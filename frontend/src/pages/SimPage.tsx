import {useCallback, useEffect, useRef, useState} from "react"
import {getCarState, resetCar, saveDataset, sendAction, socket} from "../api/socket";
import type {Car} from "../model/car";
import {checkCollision} from "../model/target";
import {useTargetStore} from "../store/targetStore";
import {
    MAP_W,
    MAP_H,
    renderTopDownTargets,
    targetsToWalls,
    computeSprites,
    renderFirstPersonWalls,
    renderFirstPersonSprites
} from "../components/target/TargetRenderer";
import {TargetManager} from "../components/target/TargetManager";
import { useTargetCreation } from "../components/target/useTargetCreation";
import { useTargetDrag } from "../components/target/useTargetDrag";

const INFER_HZ = 5;
const inferInterval = 1000 / INFER_HZ;
const ACTION_DIM = 5;
const CHUNK_SIZE = 10;
const MIN_STEPS_PER_EPISODE = 15;
const STOP_RECORD_STRIDE = 5;
const DEFAULT_TARGET_EPISODES = 8;

const INITIAL_LOCAL_W = MAP_W / 2;
const INITIAL_LOCAL_H = MAP_H / 2;

const FPS = 20
const frameInterval = 1000 / FPS

type EpisodeStep = {
    state: number[];
    envState: number[];
    action: number[];
    image?: string;
};

const intersectRaySegment = (
    x1: number,
    y1: number,
    x2: number,
    y2: number,
    x3: number,
    y3: number,
    x4: number,
    y4: number
) => {
    const denom = (y4 - y3) * (x2 - x1) - (x4 - x3) * (y2 - y1);
    if (denom === 0) return null;
    const ua = ((x4 - x3) * (y1 - y3) - (y4 - y3) * (x1 - x3)) / denom;
    const ub = ((x2 - x1) * (y1 - y3) - (y2 - y1) * (x1 - x3)) / denom;
    if (ua >= 0 && ub >= 0 && ub <= 1) {
        const ix = x1 + ua * (x2 - x1);
        const iy = y1 + ua * (y2 - y1);
        return {x: ix, y: iy};
    }
    return null;
};

// ============================================================
// SimPage 组件 - 小车模拟器主页面
// 包含俯视图(上帝视角)和第一人称视角两个画布
// ============================================================
const SimPage = () => {
    const canvasRef = useRef<HTMLCanvasElement | null>(null)
    const fpvRef = useRef<HTMLCanvasElement | null>(null);

    const { targets, updateTarget, removeTarget, selectTarget, selectedTargetId } = useTargetStore();

    const targetsRef = useRef(targets);

    useEffect(() => {
        targetsRef.current = targets;
    }, [targets]);

    const {
        selectedTargetType,
        setSelectedTargetType,
        isCreatingTarget,
        setIsCreatingTarget,
        createTarget
    } = useTargetCreation();

    useTargetDrag({
        canvasRef,
        targetsRef,
        updateTarget,
        selectTarget,
        isCreatingTarget,
        createTarget
    });

    const carState = useRef({
        x: 400,
        y: 300,
        angle: -Math.PI / 2,
    })
    const [actEnabled, setActEnabled] = useState(false)
    const [actStatus, setActStatus] = useState("ACT: off")
    const [models, setModels] = useState<{id: string; path: string; created_at?: string; updated_at?: string}[]>([])
    const [datasets, setDatasets] = useState<{id: string; path: string; size_bytes?: number; created_at?: string; updated_at?: string}[]>([])
    const [selectedModelId, setSelectedModelId] = useState("")
    const [selectedDatasetPath, setSelectedDatasetPath] = useState("")
    const [inferRunning, setInferRunning] = useState(false)
    const [trainInfo, setTrainInfo] = useState<{
        status?: string;
        epoch?: number;
        num_epochs?: number;
        avg_loss?: number;
        progress?: number;
        model_id?: string;
        error?: string;
        updated_at?: string;
    } | null>(null)
    const [trainStartStatus, setTrainStartStatus] = useState("")
    const actCommandRef = useRef<string>("stop")
    const lastCommandRef = useRef<string>("stop")
    const lastSentCommandRef = useRef<string>("stop")
    const lastSendAtRef = useRef<number>(0)
    const lastInferAtRef = useRef<number>(0)
    const inferFrameRef = useRef<number>(0)
    const localSpeedRef = useRef<number>(0)
    const inferBusyRef = useRef(false)

    const [collecting, setCollecting] = useState(false)
    const [collectStatus, setCollectStatus] = useState("采集: 未开始")
    const [targetEpisodes, setTargetEpisodes] = useState(DEFAULT_TARGET_EPISODES)
    const [collectedEpisodes, setCollectedEpisodes] = useState(0)
    const [currentStepCount, setCurrentStepCount] = useState(0)
    const episodesRef = useRef<{steps: EpisodeStep[]}[]>([])
    const currentEpisodeRef = useRef<EpisodeStep[]>([])
    const stepTickRef = useRef(0)
    const stopSampleTickRef = useRef(0)

    const applyLocalReset = useCallback(() => {
        localSpeedRef.current = 0
        carState.current = {
            x: INITIAL_LOCAL_W,
            y: INITIAL_LOCAL_H,
            angle: -Math.PI / 2,
        }
        actCommandRef.current = "stop"
        lastCommandRef.current = "stop"
        lastSentCommandRef.current = "stop"
        stopSampleTickRef.current = 0
        keys.current = {}
    }, [])

    const handleCreateTargetInFront = () => {
        const {x, y, angle} = carState.current;
        const frontX = x + Math.cos(angle) * 50;
        const frontY = y + Math.sin(angle) * 50;
        createTarget(frontX, frontY);
    };

    useEffect(() => {
        getCarState()
        socket.on('car_state', (car: Car) => {
            const newState = {
                x: car.x + INITIAL_LOCAL_W,
                y: car.y + INITIAL_LOCAL_H,
                angle: car.angle
            };
            carState.current = newState;
        });
        return () => {
            socket.off('car_state');
        }
    }, [])

    const fetchModels = useCallback(async () => {
        try {
            const res = await fetch(`/api/models`)
            const data = await res.json()
            if (Array.isArray(data?.models)) {
                setModels(data.models)
                setSelectedModelId(prev => {
                    if (prev && data.models.some((model: {id: string}) => model.id === prev)) {
                        return prev
                    }
                    return data.models[0]?.id ?? ""
                })
            } else {
                setModels([])
                setSelectedModelId("")
            }
        } catch {
            setModels([])
            setSelectedModelId("")
        }
    }, [])

    const fetchDatasets = useCallback(async () => {
        try {
            const res = await fetch(`/api/datasets`)
            const data = await res.json()
            if (Array.isArray(data?.datasets)) {
                setDatasets(data.datasets)
                setSelectedDatasetPath(prev => {
                    if (prev && data.datasets.some((dataset: {path: string}) => dataset.path === prev)) {
                        return prev
                    }
                    return data.datasets[0]?.path ?? ""
                })
            } else {
                setDatasets([])
                setSelectedDatasetPath("")
            }
        } catch {
            setDatasets([])
            setSelectedDatasetPath("")
        }
    }, [])

    useEffect(() => {
        fetchModels()
        fetchDatasets()
    }, [fetchModels, fetchDatasets])

    useEffect(() => {
        const poll = async () => {
            try {
                const res = await fetch(`/api/train/status`)
                const data = await res.json()
                setTrainInfo(data)
            } catch {
                setTrainInfo({status: "error", error: "status fetch failed"})
            }
        }
        poll()
        const timer = window.setInterval(poll, 1000)
        return () => {
            window.clearInterval(timer)
        }
    }, [])

    const startTrain = useCallback(async () => {
        if (!selectedDatasetPath) {
            setTrainStartStatus("训练: 未选择数据集")
            return
        }
        setTrainStartStatus("训练: 启动中")
        try {
            const res = await fetch(`/api/train/start`, {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify({dataset_path: selectedDatasetPath})
            })
            const data = await res.json()
            if (!res.ok) {
                throw new Error(data?.message || "start failed")
            }
            setTrainStartStatus(`训练: 已启动 (${selectedDatasetPath.split(/[\\/]/).pop() || "dataset"})`)
        } catch (err) {
            const message = err instanceof Error ? err.message : "start failed"
            setTrainStartStatus(`训练: ${message}`)
        }
    }, [selectedDatasetPath])

    const startInference = useCallback(async (modelId: string) => {
        if (!modelId) {
            setActStatus("ACT: 未选择模型")
            setInferRunning(false)
            return
        }
        try {
            const res = await fetch(`/api/infer/start`, {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify({model_id: modelId})
            })
            const data = await res.json()
            if (!res.ok) {
                throw new Error(data?.message || "start failed")
            }
            setInferRunning(true)
            setActStatus(`ACT: running (${data?.model_id || modelId})`)
        } catch (err) {
            const message = err instanceof Error ? err.message : "start failed"
            setInferRunning(false)
            setActStatus(`ACT: ${message}`)
            setActEnabled(false)
        }
    }, [])

    const stopInference = useCallback(async () => {
        if (!inferRunning) return
        try {
            await fetch(`/api/infer/stop`, {method: "POST"})
        } finally {
            setInferRunning(false)
        }
    }, [inferRunning])

    const deleteSelectedDataset = useCallback(async () => {
        if (!selectedDatasetPath) return
        const datasetId = selectedDatasetPath.split(/[\\/]/).pop() || ""
        if (!datasetId) return
        try {
            const res = await fetch(`/api/datasets/${encodeURIComponent(datasetId)}`, {method: "DELETE"})
            const data = await res.json()
            if (!res.ok) {
                throw new Error(data?.message || "delete failed")
            }
            setTrainStartStatus(`数据集: 已删除 ${datasetId}`)
            await fetchDatasets()
        } catch (err) {
            const message = err instanceof Error ? err.message : "delete failed"
            setTrainStartStatus(`数据集: ${message}`)
        }
    }, [fetchDatasets, selectedDatasetPath])

    const deleteSelectedModel = useCallback(async () => {
        if (!selectedModelId) return
        try {
            const res = await fetch(`/api/models/${encodeURIComponent(selectedModelId)}`, {method: "DELETE"})
            const data = await res.json()
            if (!res.ok) {
                throw new Error(data?.message || "delete failed")
            }
            if (inferRunning) {
                setInferRunning(false)
                setActEnabled(false)
                setActStatus("ACT: off")
            }
            await fetchModels()
            setTrainStartStatus(`模型: 已删除 ${selectedModelId}`)
        } catch (err) {
            const message = err instanceof Error ? err.message : "delete failed"
            setTrainStartStatus(`模型: ${message}`)
        }
    }, [fetchModels, inferRunning, selectedModelId])

    useEffect(() => {
        if (actEnabled) {
            startInference(selectedModelId)
        } else {
            stopInference()
        }
    }, [actEnabled, selectedModelId, startInference, stopInference])

    const keys = useRef<Record<string, boolean>>({})

    const applyLocalAction = useCallback((cmd: string) => {
        const {x, y, angle} = carState.current
        let nextAngle = angle
        let speed = localSpeedRef.current

        if (cmd === "up") {
            if (speed < 5) speed += 0.2
        }
        if (cmd === "down") {
            if (speed > -2.5) speed -= 0.2
        }
        if (cmd === "left") {
            nextAngle -= 0.05
        }
        if (cmd === "right") {
            nextAngle += 0.05
        }

        speed *= 0.95

        let nextX = x + Math.cos(nextAngle) * speed
        let nextY = y + Math.sin(nextAngle) * speed

        if (cmd === "stop") {
            speed = 0
            nextX = x
            nextY = y
        }

        if (checkCollision(nextX, nextY, MAP_W, MAP_H, targetsRef.current)) {
            speed = 0
            nextX = x
            nextY = y
        }

        localSpeedRef.current = speed
        carState.current = {x: nextX, y: nextY, angle: nextAngle}
    }, []);

    const updatePhysics = useCallback(() => {
        let cmd = "stop"
        if (actEnabled) {
            cmd = actCommandRef.current
        } else {
            if (keys.current['ArrowUp'] || keys.current['KeyW']) {
                cmd = "up"
            }
            if (keys.current['ArrowDown'] || keys.current['KeyS']) {
                cmd = "down"
            }
            if (keys.current['ArrowLeft'] || keys.current['KeyA']) {
                cmd = "left"
            }
            if (keys.current['ArrowRight'] || keys.current['KeyD']) {
                cmd = "right"
            }
            if (selectedTargetId) {
                if (keys.current['KeyQ']) {
                    const target = targets.find(t => t.id === selectedTargetId);
                    if (target && target.type === 'RECT') {
                        const currentAngle = target.angle || 0;
                        updateTarget(selectedTargetId, { angle: currentAngle - 0.05 });
                    }
                }
                if (keys.current['KeyE']) {
                    const target = targets.find(t => t.id === selectedTargetId);
                    if (target && target.type === 'RECT') {
                        const currentAngle = target.angle || 0;
                        updateTarget(selectedTargetId, { angle: currentAngle + 0.05 });
                    }
                }
                if (keys.current['Delete']) {
                    removeTarget(selectedTargetId);
                    selectTarget(null);
                }
            }
        }
        lastCommandRef.current = cmd
        const now = performance.now()
        if (!actEnabled) {
            if (cmd === "stop") {
                if (lastSentCommandRef.current !== "stop" || now - lastSendAtRef.current >= 300) {
                    sendAction(cmd)
                    lastSentCommandRef.current = cmd
                    lastSendAtRef.current = now
                }
            } else {
                sendAction(cmd)
                lastSentCommandRef.current = cmd
                lastSendAtRef.current = now
            }
        }
        applyLocalAction(cmd)
        const state = carState.current;
        if (checkCollision(state.x, state.y, MAP_W, MAP_H, targetsRef.current)) {
            if (lastSentCommandRef.current !== "stop") {
                sendAction("stop")
                lastCommandRef.current = "stop"
                lastSentCommandRef.current = "stop"
                lastSendAtRef.current = now
            }
        }
    }, [actEnabled, applyLocalAction, removeTarget, selectTarget, selectedTargetId, targets, updateTarget])

    const commandToActionVec = useCallback((cmd: string) => {
        switch (cmd) {
            case "up":
                return [1, 0, 0, 0, 0]
            case "down":
                return [0, 1, 0, 0, 0]
            case "left":
                return [0, 0, 1, 0, 0]
            case "right":
                return [0, 0, 0, 1, 0]
            default:
                return [0, 0, 0, 0, 1]
        }
    }, [])

    const getForwardDistance = useCallback((x: number, y: number, angle: number) => {
        const maxDist = Math.hypot(MAP_W, MAP_H);
        const endX = x + Math.cos(angle) * maxDist;
        const endY = y + Math.sin(angle) * maxDist;
        const walls = targetsToWalls(targetsRef.current);
        let minDist = maxDist;
        walls.forEach(wall => {
            const hit = intersectRaySegment(x, y, endX, endY, wall.x1, wall.y1, wall.x2, wall.y2);
            if (hit) {
                const dist = Math.hypot(hit.x - x, hit.y - y);
                if (dist < minDist) minDist = dist;
            }
        });
        return minDist;
    }, [])

    const getBallDistance = useCallback((x: number, y: number) => {
        const circleTargets = targetsRef.current.filter(target => target.type === 'CIRCLE')
        if (circleTargets.length === 0) {
            return Math.hypot(MAP_W, MAP_H)
        }
        let minDist = Number.POSITIVE_INFINITY
        circleTargets.forEach(target => {
            const radius = target.r || 0
            const centerDist = Math.hypot(target.x - x, target.y - y)
            const surfaceDist = Math.max(0, centerDist - radius)
            if (surfaceDist < minDist) minDist = surfaceDist
        })
        return minDist
    }, [])

    const getObservationVectors = useCallback(() => {
        const {x, y, angle} = carState.current
        const speed = localSpeedRef.current
        const ballDistance = getBallDistance(x, y)
        const state = new Array(14).fill(0)
        state[0] = x
        state[1] = y
        state[2] = angle
        state[3] = speed
        state[4] = ballDistance
        const envState = new Array(7).fill(0)
        envState[0] = x
        envState[1] = y
        envState[2] = angle
        envState[3] = speed
        envState[4] = checkCollision(x, y, MAP_W, MAP_H, targetsRef.current) ? 1 : 0
        envState[5] = getForwardDistance(x, y, angle)
        envState[6] = ballDistance
        return {state, envState}
    }, [getBallDistance, getForwardDistance])

    const packDataset = (episodes: {steps: EpisodeStep[]}[]) => {
        const states: number[][] = []
        const env_states: number[][] = []
        const actions: number[][][] = []
        const action_is_pad: number[][] = []
        const images: string[][][] = []
        let hasImages = false
        episodes.forEach(ep => {
            const steps = ep.steps
            for (let i = 0; i < steps.length; i += CHUNK_SIZE) {
                const chunkSteps = steps.slice(i, i + CHUNK_SIZE)
                const stateChunk = chunkSteps[0]?.state ?? new Array(14).fill(0)
                const envChunk = chunkSteps[0]?.envState ?? new Array(7).fill(0)
                const actionChunk: number[][] = []
                const padChunk: number[] = []
                const imageChunk: string[][] = []
                chunkSteps.forEach(step => {
                    actionChunk.push(step.action)
                    padChunk.push(0)
                    const image = step.image ?? ""
                    if (image) hasImages = true
                    imageChunk.push([image])
                })
                for (let pad = chunkSteps.length; pad < CHUNK_SIZE; pad += 1) {
                    actionChunk.push(new Array(ACTION_DIM).fill(0))
                    padChunk.push(1)
                    imageChunk.push([""])
                }
                states.push(stateChunk)
                env_states.push(envChunk)
                actions.push(actionChunk)
                action_is_pad.push(padChunk)
                images.push(imageChunk)
            }
        })
        if (hasImages) {
            return {states, env_states, actions, action_is_pad, images}
        }
        return {states, env_states, actions, action_is_pad}
    }

    const drawGrid = useCallback((ctx: CanvasRenderingContext2D, w: number, h: number) => {
        ctx.strokeStyle = '#e0e0e0'
        ctx.lineWidth = 1
        const gridSize = 50

        ctx.beginPath()
        for (let x = 0; x <= w; x += gridSize) {
            ctx.moveTo(x, 0)
            ctx.lineTo(x, h)
        }
        for (let y = 0; y <= h; y += gridSize) {
            ctx.moveTo(0, y)
            ctx.lineTo(w, y)
        }
        ctx.stroke()
    }, [])

    const drawCarBody = useCallback((ctx: CanvasRenderingContext2D) => {
        const {x, y, angle} = carState.current;
        ctx.save();
        ctx.translate(x, y);
        ctx.rotate(angle);
        ctx.fillStyle = 'blue';
        ctx.fillRect(-20, -10, 40, 20);
        ctx.fillStyle = 'yellow';
        ctx.beginPath();
        ctx.arc(15, -6, 3, 0, Math.PI * 2);
        ctx.arc(15, 6, 3, 0, Math.PI * 2);
        ctx.fill();
        ctx.fillStyle = '#2c3e50'
        ctx.fillRect(5, -8, 10, 16)
        ctx.restore();
    }, [])

    const drawTopDown = useCallback((ctx: CanvasRenderingContext2D) => {
        ctx.clearRect(0, 0, ctx.canvas.width, ctx.canvas.height)

        drawGrid(ctx, ctx.canvas.width, ctx.canvas.height)

        ctx.save()
        renderTopDownTargets(ctx, targetsRef.current, selectedTargetId);

        drawCarBody(ctx)

        const {x, y, angle} = carState.current;
        ctx.strokeStyle = 'rgba(0,0,0,0.1)';
        ctx.beginPath();
        ctx.moveTo(x, y);
        ctx.lineTo(x + Math.cos(angle - Math.PI / 6) * 100, y + Math.sin(angle - Math.PI / 6) * 100);
        ctx.moveTo(x, y);
        ctx.lineTo(x + Math.cos(angle + Math.PI / 6) * 100, y + Math.sin(angle + Math.PI / 6) * 100);
        ctx.stroke();

        ctx.restore()
    }, [drawCarBody, drawGrid, selectedTargetId])

    const drawFirstPerson = useCallback((ctx: CanvasRenderingContext2D) => {
        const w = ctx.canvas.width;
        const h = ctx.canvas.height;
        const {x, y, angle} = carState.current;

        ctx.fillStyle = '#87CEEB';
        ctx.fillRect(0, 0, w, h / 2);
        ctx.fillStyle = '#7f8c8d';
        ctx.fillRect(0, h / 2, w, h / 2);

        const fov = Math.PI / 3;
        const rayCount = w / 4;
        const rayWidth = w / rayCount;

        const walls = targetsToWalls(targetsRef.current);

        const depthBuffer = renderFirstPersonWalls(ctx, walls, x, y, angle, w, h);

        const sprites = computeSprites(targetsRef.current, x, y, angle, fov, w, h);
        renderFirstPersonSprites(ctx, sprites, depthBuffer, rayWidth, rayCount, x, y, angle, fov);
        const hasVisibleSprite = sprites.some(sprite => sprite.screenX + sprite.size > 0 && sprite.screenX - sprite.size < w);
        const hasVisibleWall = depthBuffer.some(dist => dist < Infinity);
        if (!hasVisibleSprite && !hasVisibleWall) {
            ctx.fillStyle = 'rgba(0,0,0,0.6)';
            ctx.fillRect(0, 0, w, h);
            ctx.fillStyle = '#ffffff';
            ctx.font = '16px sans-serif';
            ctx.textAlign = 'center';
            ctx.fillText('视野内无目标', w / 2, h / 2);
        }
    }, [])


    useEffect(() => {
        const canvas = canvasRef.current
        const fpv = fpvRef.current
        if (canvas == null || fpv == null) return
        const ctxTop = canvas.getContext('2d')
        const ctxFpv = fpv.getContext('2d')

        if (ctxTop == null || ctxFpv == null) return

        ctxFpv.imageSmoothingEnabled = false;

        let animationFrameId: number

        const handleKeyDown = (e: KeyboardEvent) => {
            const active = document.activeElement as HTMLElement | null
            if (active) {
                const tag = active.tagName
                if (active.isContentEditable || tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') {
                    return
                }
            }
            if (e.code.startsWith("Arrow")) {
                e.preventDefault()
            }
            keys.current[e.code] = true
        }
        const handleKeyUp = (e: KeyboardEvent) => {
            const active = document.activeElement as HTMLElement | null
            if (active) {
                const tag = active.tagName
                if (active.isContentEditable || tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') {
                    return
                }
            }
            if (e.code.startsWith("Arrow")) {
                e.preventDefault()
            }
            keys.current[e.code] = false
        }

        window.addEventListener('keydown', handleKeyDown)
        window.addEventListener('keyup', handleKeyUp)

        let lastTime = 0;

        const renderLoop = (currentTime: number) => {
            animationFrameId = window.requestAnimationFrame(renderLoop)

            const delta = currentTime - lastTime

            if (delta < frameInterval) return

            lastTime = currentTime - (delta % frameInterval)

            if (actEnabled && inferRunning) {
                const elapsed = currentTime - lastInferAtRef.current
                if (elapsed >= inferInterval && !inferBusyRef.current) {
                    inferBusyRef.current = true
                    lastInferAtRef.current = currentTime
                    const {state, envState} = getObservationVectors()
                    fetch(`/api/infer/step`, {
                        method: "POST",
                        headers: {"Content-Type": "application/json"},
                        body: JSON.stringify({state, env_state: envState})
                    })
                        .then(res => res.json())
                        .then(data => {
                            if (data?.status === "ok") {
                                const cmd = data.action || "stop"
                                actCommandRef.current = cmd
                                setActStatus(`ACT: 运行中 | 动作 ${cmd}`)
                                inferFrameRef.current += 1
                                return
                            }
                            if (data?.message) {
                                setActStatus(`ACT: ${data.message}`)
                                actCommandRef.current = "stop"
                                setInferRunning(false)
                                setActEnabled(false)
                            }
                        })
                        .catch(err => {
                            const message = err instanceof Error ? err.message : "infer failed"
                            setActStatus(`ACT: ${message}`)
                            actCommandRef.current = "stop"
                            setInferRunning(false)
                            setActEnabled(false)
                        })
                        .finally(() => {
                            inferBusyRef.current = false
                        })
                }
            }
            updatePhysics()
            drawTopDown(ctxTop)
            drawFirstPerson(ctxFpv)
            if (collecting) {
                const cmd = lastCommandRef.current
                let shouldRecord = true
                if (cmd === "stop") {
                    stopSampleTickRef.current += 1
                    shouldRecord = stopSampleTickRef.current % STOP_RECORD_STRIDE === 1
                } else {
                    stopSampleTickRef.current = 0
                }
                if (shouldRecord) {
                    const {state, envState} = getObservationVectors()
                    const action = commandToActionVec(cmd)
                    const image = fpvRef.current?.toDataURL('image/png')
                    currentEpisodeRef.current.push({state, envState, action, image})
                    stepTickRef.current += 1
                    if (stepTickRef.current % 10 === 0) {
                        setCurrentStepCount(currentEpisodeRef.current.length)
                    }
                }
            }
        }

        animationFrameId = window.requestAnimationFrame(renderLoop)

        return () => {
            window.removeEventListener('keydown', handleKeyDown)
            window.removeEventListener('keyup', handleKeyUp)

            window.cancelAnimationFrame(animationFrameId)
        }
    }, [actEnabled, inferRunning, collecting, commandToActionVec, drawFirstPerson, drawTopDown, getObservationVectors, updatePhysics])

    const sendCommand = (cmd: string) => {
        keys.current[cmd] = true
        setTimeout(() => {
            keys.current[cmd] = false
        }, 200)
    }

    const startCollect = () => {
        episodesRef.current = []
        currentEpisodeRef.current = []
        setCollectedEpisodes(0)
        setCurrentStepCount(0)
        stepTickRef.current = 0
        stopSampleTickRef.current = 0
        setCollecting(true)
        setCollectStatus(`采集: 进行中（每轮至少${MIN_STEPS_PER_EPISODE}条）`)
    }

    const stopCollect = async () => {
        const currentLen = currentEpisodeRef.current.length
        if (currentLen >= MIN_STEPS_PER_EPISODE) {
            episodesRef.current.push({steps: currentEpisodeRef.current.slice()})
            currentEpisodeRef.current = []
        } else if (currentLen > 0) {
            currentEpisodeRef.current = []
            setCollectStatus(`采集: 最后一轮不足${MIN_STEPS_PER_EPISODE}条，已忽略`)
        }
        setCollectedEpisodes(episodesRef.current.length)
        setCurrentStepCount(0)
        const episodes = episodesRef.current
        if (episodes.length === 0) {
            setCollecting(false)
            setCollectStatus("采集: 无数据")
            return
        }
        setCollectStatus("采集: 保存中")
        try {
            const payload = packDataset(episodes)
            const res = await saveDataset(payload)
            if (res && typeof res.path === "string") {
                setCollectStatus(`采集: 已保存 ${res.path}`)
                setSelectedDatasetPath(res.path)
                fetchDatasets()
            } else {
                setCollectStatus("采集: 已保存")
                fetchDatasets()
            }
        } catch {
            setCollectStatus("采集: 保存失败")
        }
        setCollecting(false)
    }

    const handleResetSave = () => {
        if (collecting) {
            if (episodesRef.current.length >= targetEpisodes) {
                setCollectStatus("采集: 已达目标")
                setCollecting(false)
                setCurrentStepCount(0)
                applyLocalReset()
                resetCar()
                return
            }
            const currentLen = currentEpisodeRef.current.length
            if (currentLen < MIN_STEPS_PER_EPISODE) {
                setCollectStatus(`采集: 当前回合至少${MIN_STEPS_PER_EPISODE}条`)
                return
            }
            episodesRef.current.push({steps: currentEpisodeRef.current.slice()})
            currentEpisodeRef.current = []
            const count = episodesRef.current.length
            setCollectedEpisodes(count)
            setCurrentStepCount(0)
            if (count >= targetEpisodes) {
                setCollectStatus("采集: 已达目标")
                setCollecting(false)
            } else {
                setCollectStatus(`采集: 已记录 ${count}/${targetEpisodes}`)
            }
        }
        setCurrentStepCount(0)
        applyLocalReset()
        resetCar()
    }

    return (
        <div style={{
            display: 'flex',
            flexDirection: 'column',
            gap: '12px',
            padding: '12px 16px',
            height: '100vh',
            boxSizing: 'border-box',
            overflow: 'hidden'
        }}>
            <h1 style={{textAlign: 'center', margin: 0}}>小车模拟器</h1>
            <div style={{
                display: 'flex',
                flexDirection: 'row',
                gap: '20px',
                flex: 1,
                alignItems: 'stretch'
            }}>
                <div style={{
                    flex: '0 0 260px',
                    display: 'flex',
                    flexDirection: 'column',
                    height: '100%'
                }}>
                    <div style={{border: '2px solid #333', borderRadius: 8, padding: '12px 16px 12px 12px', background: '#f9f9f9', overflowY: 'auto', boxSizing: 'border-box', height: '100%'}}>
                        <TargetManager
                            onCreateInFront={handleCreateTargetInFront}
                            isCreatingTarget={isCreatingTarget}
                            onToggleCreating={setIsCreatingTarget}
                            selectedTargetType={selectedTargetType}
                            onTargetTypeChange={setSelectedTargetType}
                        />
                    </div>
                </div>
                <div style={{
                    flex: 1,
                    display: 'flex',
                    flexDirection: 'column',
                    height: '100%'
                }}>
                    <div style={{border: '2px solid #333', borderRadius: 8, background: '#f9f9f9', padding: 12, display: 'flex', flexDirection: 'column', gap: 12, height: '100%'}}>
                        <div style={{fontWeight: 600}}>俯视地图</div>
                        <div style={{position: 'relative', alignSelf: 'center'}}>
                            <canvas
                                ref={canvasRef}
                                width={800}
                                height={600}
                                style={{background: '#ffffff', display: 'block', borderRadius: 4}}
                            />
                            <div style={{
                                position: 'absolute',
                                top: 8,
                                left: 8,
                                background: 'rgba(255,255,255,0.85)',
                                padding: 6,
                                borderRadius: 4,
                                fontSize: 12
                            }}>
                                使用 WASD 或 方向键 移动<br/>
                                使用 QE 键旋转选中的目标物<br/>
                                选中目标物后按 Delete 键删除
                            </div>
                        </div>
                        <div style={{display: 'flex', gap: '10px', flexWrap: 'wrap', justifyContent: 'center', alignItems: 'center'}}>
                            <button onClick={() => sendCommand('ArrowUp')}>指令: 前进</button>
                            <button onClick={() => sendCommand('ArrowLeft')}>指令: 左转</button>
                            <button onClick={() => sendCommand('ArrowRight')}>指令: 右转</button>
                            <button onClick={() => sendCommand('ArrowDown')}>指令: 后退</button>
                            <button onClick={() => {
                                applyLocalReset()
                                resetCar()
                            }}>复位</button>
                        </div>
                    </div>
                </div>
                <div style={{
                    flex: '0 0 360px',
                    display: 'flex',
                    flexDirection: 'column',
                    height: '100%'
                }}>
                    <div style={{border: '2px solid #333', borderRadius: 8, padding: '10px', background: '#f9f9f9', display: 'flex', flexDirection: 'column', gap: 8, color: '#333', height: '100%', overflowY: 'auto', minHeight: 0}}>
                        <div style={{fontWeight: 600}}>车载摄像头</div>
                        <canvas ref={fpvRef} width={320} height={240}
                                style={{background: '#000', border: '2px solid #333', borderRadius: 4, alignSelf: 'center'}}/>
                        <div style={{fontSize: 12, color: '#555'}}>
                            说明：右侧画面是根据左侧地图实时计算生成的伪3D视角。
                        </div>
                        <div style={{height: 1, background: '#ddd'}}/>
                        <div style={{fontWeight: 600}}>ACT 推理</div>
                        <div style={{display: 'flex', gap: '6px', alignItems: 'center', flexWrap: 'wrap'}}>
                            <select
                                value={selectedModelId}
                                onChange={(e) => setSelectedModelId(e.target.value)}
                                style={{flex: 1, minWidth: 140}}
                            >
                                <option value="">未选择模型</option>
                                {models.map(model => (
                                    <option key={model.id} value={model.id}>{model.id}</option>
                                ))}
                            </select>
                            <button onClick={fetchModels}>刷新</button>
                            <button onClick={deleteSelectedModel} disabled={!selectedModelId}>删除模型</button>
                        </div>
                        <button
                            onClick={() => setActEnabled(v => {
                                const next = !v
                                setActStatus(`ACT: ${next ? "on" : "off"}`)
                                return next
                            })}
                        >
                            切换 ACT
                        </button>
                        <div style={{fontSize: 12, opacity: 0.9}}>{actStatus}</div>
                        <div style={{height: 1, background: '#ddd'}}/>
                        <div style={{fontWeight: 600}}>采集 / 训练</div>
                        <div style={{display: 'flex', gap: '8px', flexWrap: 'wrap'}}>
                            <button onClick={startCollect}>开始采集</button>
                            <button onClick={stopCollect}>结束采集</button>
                            <button onClick={handleResetSave}>复位保存</button>
                            <button onClick={startTrain}>开始训练</button>
                        </div>
                        <div style={{display: 'flex', gap: '6px', alignItems: 'center', flexWrap: 'wrap'}}>
                            <select
                                value={selectedDatasetPath}
                                onChange={(e) => setSelectedDatasetPath(e.target.value)}
                                style={{flex: 1, minWidth: 220}}
                            >
                                <option value="">未选择数据集</option>
                                {datasets.map(dataset => (
                                    <option key={dataset.id} value={dataset.path}>{dataset.id}</option>
                                ))}
                            </select>
                            <button onClick={fetchDatasets}>刷新</button>
                            <button onClick={deleteSelectedDataset} disabled={!selectedDatasetPath}>删除数据集</button>
                        </div>
                        <div style={{fontSize: 11, opacity: 0.9, lineHeight: 1.2}}>
                            已完成回合 {collectedEpisodes}/{targetEpisodes}
                        </div>
                        <div style={{fontSize: 11, opacity: 0.9, lineHeight: 1.2}}>
                            当前回合步数 {currentStepCount}
                        </div>
                        <label style={{display: 'flex', alignItems: 'center', gap: '6px', fontSize: 11}}>
                            目标回合（建议8）
                            <input
                                type="number"
                                min={1}
                                value={targetEpisodes}
                                disabled={collecting}
                                onChange={(e) => {
                                    const next = Number(e.target.value)
                                    if (!Number.isNaN(next) && next > 0) {
                                        setTargetEpisodes(next)
                                    }
                                }}
                                style={{width: 80}}
                            />
                        </label>
                        <div style={{fontSize: 11, opacity: 0.9, lineHeight: 1.2}}>
                            {collectStatus}
                        </div>
                        <div style={{fontSize: 11, opacity: 0.9, lineHeight: 1.2}}>
                            训练状态 {trainInfo?.status || "unknown"}
                        </div>
                        <div style={{fontSize: 11, opacity: 0.9, lineHeight: 1.2}}>
                            训练进度 {trainInfo?.epoch ?? 0}/{trainInfo?.num_epochs ?? 0}
                        </div>
                        <div style={{fontSize: 11, opacity: 0.9, lineHeight: 1.2}}>
                            平均损失 {typeof trainInfo?.avg_loss === "number" ? trainInfo.avg_loss.toFixed(4) : "-"}
                        </div>
                        {trainStartStatus ? (
                            <div style={{fontSize: 11, opacity: 0.9, lineHeight: 1.2}}>
                                {trainStartStatus}
                            </div>
                        ) : null}
                        <div style={{fontSize: 11, opacity: 0.9, lineHeight: 1.2}}>
                            流程：开始采集 → 每轮至少采集15条 → 复位保存 → 累计约8轮后结束采集
                        </div>
                    </div>
                </div>
            </div>


        </div>
    )
}

export default SimPage
