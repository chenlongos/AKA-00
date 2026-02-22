import {useCallback, useEffect, useRef, useState} from "react"
import {actInfer, getCarState, resetCar, saveDataset, sendAction, socket} from "../api/socket";
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
const ACTION_DIM = 3;
const CHUNK_SIZE = 50;

const INITIAL_LOCAL_W = MAP_W / 2;
const INITIAL_LOCAL_H = MAP_H / 2;

const FPS = 30
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
    const actCommandRef = useRef<string>("stop")
    const lastCommandRef = useRef<string>("stop")
    const lastSentCommandRef = useRef<string>("stop")
    const lastSendAtRef = useRef<number>(0)
    const lastInferAtRef = useRef<number>(0)
    const inferFrameRef = useRef<number>(0)
    const localSpeedRef = useRef<number>(0)

    const [collecting, setCollecting] = useState(false)
    const [collectStatus, setCollectStatus] = useState("采集: 未开始")
    const [targetEpisodes, setTargetEpisodes] = useState(50)
    const [collectedEpisodes, setCollectedEpisodes] = useState(0)
    const [currentStepCount, setCurrentStepCount] = useState(0)
    const episodesRef = useRef<{steps: EpisodeStep[]}[]>([])
    const currentEpisodeRef = useRef<EpisodeStep[]>([])
    const stepTickRef = useRef(0)

    const handleCreateTargetInFront = () => {
        const {x, y, angle} = carState.current;
        const frontX = x + Math.cos(angle) * 50;
        const frontY = y + Math.sin(angle) * 50;
        createTarget(frontX, frontY);
    };

    const mapActionToCommand = (vec: number[]) => {
        if (!Array.isArray(vec) || vec.length === 0) return "stop"
        const v0 = vec[0] ?? 0
        const v1 = vec[1] ?? 0
        const magnitude = Math.abs(v0) + Math.abs(v1)
        if (magnitude < 0.1) return "stop"
        if (Math.abs(v0) >= Math.abs(v1)) {
            return v0 >= 0 ? "up" : "down"
        }
        return v1 >= 0 ? "right" : "left"
    }

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
        socket.on('act_action', (payload: {action?: number[][][]; error?: string}) => {
            if (payload?.error) {
                setActStatus(`ACT: ${payload.error}`)
                actCommandRef.current = "stop"
                return
            }
            const action = payload?.action
            if (!action || action.length === 0 || action[0].length === 0) {
                setActStatus("ACT: empty")
                actCommandRef.current = "stop"
                return
            }
            const cmd = mapActionToCommand(action[0][0])
            actCommandRef.current = cmd
            setActStatus(`ACT: ${cmd}`)
        })
        return () => {
            socket.off('car_state');
            socket.off('act_action');
        }
    }, [])

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
                return [1, 0, 0]
            case "down":
                return [-1, 0, 0]
            case "left":
                return [0, -1, 0]
            case "right":
                return [0, 1, 0]
            default:
                return [0, 0, 0]
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

    const getObservationVectors = useCallback(() => {
        const {x, y, angle} = carState.current
        const state = new Array(14).fill(0)
        state[0] = x
        state[1] = y
        state[2] = angle
        const envState = new Array(6).fill(0)
        envState[0] = x
        envState[1] = y
        envState[2] = angle
        envState[3] = 0
        envState[4] = checkCollision(x, y, MAP_W, MAP_H, targetsRef.current) ? 1 : 0
        envState[5] = getForwardDistance(x, y, angle)
        return {state, envState}
    }, [getForwardDistance])

    const buildInferencePayload = useCallback(() => {
        const {state, envState} = getObservationVectors()
        const frameId = inferFrameRef.current
        inferFrameRef.current += 1
        return {
            observation: {state, environment_state: envState},
            meta: {frame_id: frameId, index: frameId}
        }
    }, [getObservationVectors])

    const packDataset = (episodes: {steps: EpisodeStep[]}[]) => {
        const states: number[][][] = []
        const env_states: number[][][] = []
        const actions: number[][][] = []
        const action_is_pad: number[][] = []
        const images: string[][][] = []
        let hasImages = false
        episodes.forEach(ep => {
            const steps = ep.steps
            for (let i = 0; i < steps.length; i += CHUNK_SIZE) {
                const chunkSteps = steps.slice(i, i + CHUNK_SIZE)
                const stateChunk: number[][] = []
                const envChunk: number[][] = []
                const actionChunk: number[][] = []
                const padChunk: number[] = []
                const imageChunk: string[][] = []
                chunkSteps.forEach(step => {
                    stateChunk.push(step.state)
                    envChunk.push(step.envState)
                    actionChunk.push(step.action)
                    padChunk.push(0)
                    const image = step.image ?? ""
                    if (image) hasImages = true
                    imageChunk.push([image])
                })
                for (let pad = chunkSteps.length; pad < CHUNK_SIZE; pad += 1) {
                    stateChunk.push(new Array(14).fill(0))
                    envChunk.push(new Array(6).fill(0))
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

            if (actEnabled) {
                const elapsed = currentTime - lastInferAtRef.current
                if (elapsed >= inferInterval) {
                    lastInferAtRef.current = currentTime
                    actInfer(buildInferencePayload())
                }
            }
            updatePhysics()
            drawTopDown(ctxTop)
            drawFirstPerson(ctxFpv)
            if (collecting) {
                const {state, envState} = getObservationVectors()
                const action = commandToActionVec(lastCommandRef.current)
                const image = fpvRef.current?.toDataURL('image/png')
                currentEpisodeRef.current.push({state, envState, action, image})
                stepTickRef.current += 1
                if (stepTickRef.current % 10 === 0) {
                    setCurrentStepCount(currentEpisodeRef.current.length)
                }
            }
        }

        animationFrameId = window.requestAnimationFrame(renderLoop)

        return () => {
            window.removeEventListener('keydown', handleKeyDown)
            window.removeEventListener('keyup', handleKeyUp)

            window.cancelAnimationFrame(animationFrameId)
        }
    }, [actEnabled, buildInferencePayload, collecting, commandToActionVec, drawFirstPerson, drawTopDown, getObservationVectors, updatePhysics])

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
        setCollecting(true)
        setCollectStatus("采集: 进行中")
    }

    const stopCollect = async () => {
        if (currentEpisodeRef.current.length > 0) {
            episodesRef.current.push({steps: currentEpisodeRef.current})
            currentEpisodeRef.current = []
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
            } else {
                setCollectStatus("采集: 已保存")
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
                resetCar()
                return
            }
            if (currentEpisodeRef.current.length > 0) {
                episodesRef.current.push({steps: currentEpisodeRef.current})
                currentEpisodeRef.current = []
            } else {
                const {state, envState} = getObservationVectors()
                const action = commandToActionVec(lastCommandRef.current)
                const image = fpvRef.current?.toDataURL('image/png')
                currentEpisodeRef.current.push({state, envState, action, image})
                episodesRef.current.push({steps: currentEpisodeRef.current})
                currentEpisodeRef.current = []
            }
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
        resetCar()
    }

    return (
        <div style={{
            display: 'flex',
            flexDirection: 'column',
            gap: '20px',
            padding: '20px',
            height: '100vh',
            boxSizing: 'border-box',
            overflow: 'hidden'
        }}>
            <h1 style={{textAlign: 'center'}}>小车模拟器</h1>
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
                    flexDirection: 'column'
                }}>
                    <div style={{border: '2px solid #333', borderRadius: 8, padding: '12px 16px 12px 12px', background: '#f9f9f9', height: '100%', overflowY: 'auto', boxSizing: 'border-box'}}>
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
                    flexDirection: 'column'
                }}>
                    <div style={{border: '2px solid #333', borderRadius: 8, background: '#f9f9f9', padding: 12, height: '100%', display: 'flex', flexDirection: 'column', gap: 12}}>
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
                            <button onClick={() => resetCar()}>复位</button>
                        </div>
                    </div>
                </div>
                <div style={{
                    flex: '0 0 360px',
                    display: 'flex',
                    flexDirection: 'column'
                }}>
                    <div style={{border: '2px solid #333', borderRadius: 8, padding: '12px', background: '#f9f9f9', height: '100%', display: 'flex', flexDirection: 'column', gap: 12, color: '#333'}}>
                        <div style={{fontWeight: 600}}>车载摄像头</div>
                        <canvas ref={fpvRef} width={320} height={240}
                                style={{background: '#000', border: '2px solid #333', borderRadius: 4, alignSelf: 'center'}}/>
                        <div style={{fontSize: 12, color: '#555'}}>
                            说明：右侧画面是根据左侧地图实时计算生成的伪3D视角。
                        </div>
                        <div style={{height: 1, background: '#ddd'}}/>
                        <div style={{fontWeight: 600}}>ACT 推理</div>
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
                        </div>
                        <div style={{fontSize: 12, opacity: 0.9}}>
                            已完成回合 {collectedEpisodes}/{targetEpisodes}
                        </div>
                        <div style={{fontSize: 12, opacity: 0.9}}>
                            当前回合步数 {currentStepCount}
                        </div>
                        <label style={{display: 'flex', alignItems: 'center', gap: '6px', fontSize: 12}}>
                            目标回合
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
                        <div style={{fontSize: 12, opacity: 0.9}}>
                            {collectStatus}
                        </div>
                        <div style={{fontSize: 12, opacity: 0.9}}>
                            流程：开始采集 → 操作小车 → 复位保存 → 自动保存一条数据 → 调整场景 → 结束采集
                        </div>
                    </div>
                </div>
            </div>


        </div>
    )
}

export default SimPage
