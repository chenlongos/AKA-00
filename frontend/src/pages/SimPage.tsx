import {useEffect, useRef, useState} from "react"
import {actInfer, getCarState, resetCar, sendAction, socket} from "../api/socket";
import type {Car} from "../model/car";
import type {Obstacle} from "../model/obstacle";
import {isPointInObstacle} from "../model/obstacle";
import {useObstacleStore} from "../store/obstacleStore";
import {
    renderTopDownObstacles,
    obstaclesToWalls,
    computeSprites,
    renderFirstPersonWalls,
    renderFirstPersonSprites
} from "../components/obstacle/ObstacleRenderer";
import {ObstacleManager} from "../components/obstacle/ObstacleManager";

const MAP_W = 800; 
const MAP_H = 600; 

// 坐标偏移量：将后端坐标(中心为原点)转换为前端坐标(左上角为原点)
const INITIAL_LOCAL_W = MAP_W / 2;
const INITIAL_LOCAL_H = MAP_H / 2;

// 渲染帧率设置
const FPS = 20
const frameInterval = 1000 / FPS

// ============================================================
// SimPage 组件 - 小车模拟器主页面
// 包含俯视图(上帝视角)和第一人称视角两个画布
// ============================================================
const SimPage = () => {
    // ----------------------------------------
    // Ref 引用：用于访问 DOM 元素
    // ----------------------------------------
    const canvasRef = useRef<HTMLCanvasElement | null>(null)   // 俯视图画布引用
    const fpvRef = useRef<HTMLCanvasElement | null>(null);     // 第一人称视角画布引用

    // ----------------------------------------
    // 状态管理：障碍物数据
    // ----------------------------------------
    // 使用 zustand store 管理障碍物状态
    const { obstacles, setObstacles, updateObstacle, removeObstacle, selectObstacle, selectedObstacleId } = useObstacleStore();
    
    // 使用 useRef 存储障碍物的实时引用
    // 目的：解决渲染循环(每帧调用)中访问最新状态的闭包问题
    // 原理：obstaclesRef.current 始终指向最新的 obstacles 数组
    const obstaclesRef = useRef(obstacles);

    // 建立 zustand store 和 useRef 之间的同步
    // 当 obstacles 更新时，obstaclesRef.current 也会同步更新
    useEffect(() => {
        obstaclesRef.current = obstacles;
    }, [obstacles]);
    
    // 拖拽状态管理
    const draggingRef = useRef({
        isDragging: false,
        obstacleId: null as string | null
    });
    
    // 障碍物创建状态（用于画布点击创建）
    const [selectedObstacleType, _setSelectedObstacleType] = useState<'RECT' | 'CIRCLE'>('RECT');
    const [isCreatingObstacle, _setIsCreatingObstacle] = useState(false);

    // ----------------------------------------
    // 状态管理：小车状态 (使用 ref 避免频繁重渲染)
    // ----------------------------------------
    const carState = useRef({
        x: 400,          // 初始 X 坐标
        y: 300,          // 初始 Y 坐标
        angle: -Math.PI / 2, // 初始角度 (弧度)，-PI/2 朝上
    })
    const [actEnabled, setActEnabled] = useState(false)
    const [actStatus, setActStatus] = useState("ACT: off")
    const actCommandRef = useRef<string>("stop")

    const handleCreateObstacleInFront = () => {
        const {x, y, angle} = carState.current;
        const frontX = x + Math.cos(angle) * 50;
        const frontY = y + Math.sin(angle) * 50;
        createObstacle(frontX, frontY);
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

    // eslint-disable-next-line react-hooks/exhaustive-deps
    const updatePhysics = () => {
        if (actEnabled) {
            sendAction(actCommandRef.current)
            const state = carState.current
            if (checkCollision(state.x, state.y)) {
                sendAction("stop")
            }
            return
        }
        // 前进 / 后退
        if (keys.current['ArrowUp'] || keys.current['KeyW']) {
            sendAction("up")
        }
        if (keys.current['ArrowDown'] || keys.current['KeyS']) {
            sendAction("down")
        }
        if (keys.current['ArrowLeft'] || keys.current['KeyA']) {
            sendAction("left")
        }
        if (keys.current['ArrowRight'] || keys.current['KeyD']) {
            sendAction("right")
        }
        
        // 旋转选中的障碍物
        if (selectedObstacleId) {
            if (keys.current['KeyQ']) {
                // 向左旋转（逆时针）
                const obstacle = obstacles.find(obs => obs.id === selectedObstacleId);
                if (obstacle && obstacle.type === 'RECT') {
                    const currentAngle = obstacle.angle || 0;
                    updateObstacle(selectedObstacleId, { angle: currentAngle - 0.05 });
                }
            }
            if (keys.current['KeyE']) {
                // 向右旋转（顺时针）
                const obstacle = obstacles.find(obs => obs.id === selectedObstacleId);
                if (obstacle && obstacle.type === 'RECT') {
                    const currentAngle = obstacle.angle || 0;
                    updateObstacle(selectedObstacleId, { angle: currentAngle + 0.05 });
                }
            }
            if (keys.current['Delete']) {
                // 删除选中的障碍物
                removeObstacle(selectedObstacleId);
                selectObstacle(null);
            }
        }
        
        const state = carState.current;
        if (checkCollision(state.x, state.y)) {
            sendAction("stop")
        }
    }

    // ============================================================
    // 碰撞检测函数
    // 参数：x, y - 要检测的坐标点
    // 返回：boolean - true 表示发生碰撞，false 表示安全
    // ============================================================
    const checkCollision = (x: number, y: number) => {
        // 边界检查：超出地图范围视为碰撞
        if (x < 0 || x > MAP_W || y < 0 || y > MAP_H) return true;
        // 障碍物检查：使用 obstaclesRef.current 获取最新的障碍物数据
        return obstaclesRef.current.some(obs => isPointInObstacle(x, y, obs));
    };

    const buildObservation = () => {
        const {x, y, angle} = carState.current
        const state = new Array(14).fill(0)
        state[0] = x
        state[1] = y
        state[2] = angle
        const envState = [x, y, angle, 0, 0, 0]
        return {observation: {state, environment_state: envState}}
    }

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

    const drawGrid = (ctx: CanvasRenderingContext2D, w: number, h: number) => {
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
    }

    const drawCarBody = (ctx: CanvasRenderingContext2D) => {
        const {x, y, angle} = carState.current;
        ctx.save();
        ctx.translate(x, y);
        ctx.rotate(angle);
        ctx.fillStyle = 'blue';
        ctx.fillRect(-20, -10, 40, 20);
        ctx.fillStyle = 'yellow'; // 车灯
        ctx.beginPath();
        ctx.arc(15, -6, 3, 0, Math.PI * 2);
        ctx.arc(15, 6, 3, 0, Math.PI * 2);
        ctx.fill();
        // 挡风玻璃
        ctx.fillStyle = '#2c3e50'
        ctx.fillRect(5, -8, 10, 16)
        ctx.restore();
    }

    // ============================================================
    // 俯视图绘制函数 (上帝视角)
    // 参数：ctx - Canvas 2D 绘图上下文
    // 功能：绘制网格、障碍物、小车和方向指示线
    // ============================================================
    const drawTopDown = (ctx: CanvasRenderingContext2D) => {
        // 清空画布
        ctx.clearRect(0, 0, ctx.canvas.width, ctx.canvas.height)

        // 绘制背景网格 (模拟地面)
        drawGrid(ctx, ctx.canvas.width, ctx.canvas.height)

        // 保存当前绘图状态
        ctx.save()

        // 2. 画障碍物：使用提取的渲染函数
        renderTopDownObstacles(ctx, obstaclesRef.current, selectedObstacleId);

        // 3. 绘制小车 (此时原点就是车身中心)
        drawCarBody(ctx)

        const {x, y, angle} = carState.current;
        ctx.strokeStyle = 'rgba(0,0,0,0.1)';
        ctx.beginPath();
        ctx.moveTo(x, y);
        ctx.lineTo(x + Math.cos(angle - Math.PI / 6) * 100, y + Math.sin(angle - Math.PI / 6) * 100);
        ctx.moveTo(x, y);
        ctx.lineTo(x + Math.cos(angle + Math.PI / 6) * 100, y + Math.sin(angle + Math.PI / 6) * 100);
        ctx.stroke();

        // 恢复绘图状态
        ctx.restore()
    }

    // 检查鼠标点击是否在障碍物内
    const getObstacleAtPosition = (x: number, y: number) => {
        return obstaclesRef.current.find(obs => isPointInObstacle(x, y, obs));
    };
    
    // 创建新障碍物（在画布点击时使用）
    const createObstacle = (x: number, y: number) => {
        const newObstacle: Obstacle = {
            id: `obs_${Date.now()}_${Math.floor(Math.random() * 1000)}`,
            type: selectedObstacleType,
            x,
            y,
            w: selectedObstacleType === 'RECT' ? 50 : undefined,
            h: selectedObstacleType === 'RECT' ? 30 : undefined,
            r: selectedObstacleType === 'CIRCLE' ? 20 : undefined,
            color: selectedObstacleType === 'RECT' ? '#8B4513' : '#2E8B57',
            angle: 0
        };
        
        setObstacles([...obstacles, newObstacle]);
    };

    // 帧率控制变量
    const drawFirstPerson = (ctx: CanvasRenderingContext2D) => {
        const w = ctx.canvas.width;
        const h = ctx.canvas.height;
        const {x, y, angle} = carState.current;

        // 天空和地面
        ctx.fillStyle = '#87CEEB';
        ctx.fillRect(0, 0, w, h / 2);
        ctx.fillStyle = '#7f8c8d';
        ctx.fillRect(0, h / 2, w, h / 2);

        const fov = Math.PI / 3;
        const rayCount = w / 4;
        const rayWidth = w / rayCount;

        // 每帧重新计算墙段（确保障碍物位置更新时能正确渲染）
        const walls = obstaclesToWalls(obstaclesRef.current);

        // 渲染墙体并获取深度缓冲
        const depthBuffer = renderFirstPersonWalls(ctx, walls, x, y, angle, w, h);

        // 计算精灵数据
        const sprites = computeSprites(obstaclesRef.current, x, y, angle, fov, w, h);

        // 渲染精灵
        renderFirstPersonSprites(ctx, sprites, depthBuffer, rayWidth, rayCount);
    }


    useEffect(() => {
        const canvas = canvasRef.current
        const fpv = fpvRef.current
        if (canvas == null || fpv == null) return
        const ctxTop = canvas.getContext('2d')
        const ctxFpv = fpv.getContext('2d')

        if (ctxTop == null || ctxFpv == null) return

        // 禁用平滑处理，让像素风更清晰（可选）
        ctxFpv.imageSmoothingEnabled = false;

        let animationFrameId: number

        // 1. 监听键盘事件
        const handleKeyDown = (e: KeyboardEvent) => {
            keys.current[e.code] = true
        }
        const handleKeyUp = (e: KeyboardEvent) => {
            keys.current[e.code] = false
        }

        window.addEventListener('keydown', handleKeyDown)
        window.addEventListener('keyup', handleKeyUp)
        
        // 2. 监听鼠标事件（用于拖拽障碍物）
        let handleMouseDown: ((e: MouseEvent) => void) | null = null;
        let handleMouseMove: ((e: MouseEvent) => void) | null = null;
        let handleMouseUp: (() => void) | null = null;
        
        if (canvas) {
            handleMouseDown = (e: MouseEvent) => {
                    const rect = canvas.getBoundingClientRect();
                    const x = e.clientX - rect.left;
                    const y = e.clientY - rect.top;
                    
                    if (isCreatingObstacle) {
                        // 创建新障碍物
                        createObstacle(x, y);
                    } else {
                        const clickedObstacle = getObstacleAtPosition(x, y);
                        if (clickedObstacle) {
                            selectObstacle(clickedObstacle.id);
                            draggingRef.current = {
                                isDragging: true,
                                obstacleId: clickedObstacle.id
                            };
                        } else {
                            selectObstacle(null);
                        }
                    }
                };
            
            handleMouseMove = (e: MouseEvent) => {
                if (draggingRef.current.isDragging && draggingRef.current.obstacleId) {
                    const rect = canvas.getBoundingClientRect();
                    const x = e.clientX - rect.left;
                    const y = e.clientY - rect.top;
                    
                    // 更新障碍物位置
                    updateObstacle(draggingRef.current.obstacleId, {x, y});
                }
            };
            
            handleMouseUp = () => {
                draggingRef.current = {
                    isDragging: false,
                    obstacleId: null
                };
            };
            
            canvas.addEventListener('mousedown', handleMouseDown);
            canvas.addEventListener('mousemove', handleMouseMove);
            canvas.addEventListener('mouseup', handleMouseUp);
            canvas.addEventListener('mouseleave', handleMouseUp);
        }

        let lastTime = 0;

        // 2. 核心渲染循环
        const renderLoop = (currentTime: number) => {
            animationFrameId = window.requestAnimationFrame(renderLoop)

            const delta = currentTime - lastTime

            if (delta < frameInterval) return

            lastTime = currentTime - (delta % frameInterval)

            if (actEnabled) {
                actInfer(buildObservation())
            }
            updatePhysics()
            drawTopDown(ctxTop)
            drawFirstPerson(ctxFpv)
        }

        animationFrameId = window.requestAnimationFrame(renderLoop)

        // 清理函数
        return () => {
            window.removeEventListener('keydown', handleKeyDown)
            window.removeEventListener('keyup', handleKeyUp)
            if (canvas && handleMouseDown && handleMouseMove && handleMouseUp) {
                canvas.removeEventListener('mousedown', handleMouseDown);
                canvas.removeEventListener('mousemove', handleMouseMove);
                canvas.removeEventListener('mouseup', handleMouseUp);
                canvas.removeEventListener('mouseleave', handleMouseUp);
            }
            window.cancelAnimationFrame(animationFrameId)
        }
    }, [actEnabled, drawTopDown, drawFirstPerson, updatePhysics, createObstacle, isCreatingObstacle, updateObstacle, handleCreateObstacleInFront])

    // --- 外部指令模拟 ---
    const sendCommand = (cmd: string) => {
        keys.current[cmd] = true
        setTimeout(() => {
            keys.current[cmd] = false
        }, 200)
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
                flex: 1
            }}>
                {/* 左侧：障碍物管理 */}
                <ObstacleManager 
                    onCreateInFront={handleCreateObstacleInFront}
                    isCreatingObstacle={isCreatingObstacle}
                    onToggleCreating={(creating) => _setIsCreatingObstacle(creating)}
                    selectedObstacleType={selectedObstacleType}
                    onObstacleTypeChange={(type) => _setSelectedObstacleType(type)}
                />
                
                
                {/* 右侧：画布和控制按钮 */}
                <div style={{
                    flex: 1,
                    display: 'flex',
                    flexDirection: 'column',
                    gap: '20px',
                    alignItems: 'center'
                }}>
                    <div style={{display: 'flex', flexDirection: 'row', gap: '20px'}}>
                        <div style={{display: 'flex', flexDirection: 'column', alignItems: 'center', gap: '20px'}}>
                            {/* 左侧：上帝视角 */}
                            <div style={{position: 'relative', border: '2px solid #333'}}>
                                <canvas
                                    ref={canvasRef}
                                    width={800}
                                    height={600}
                                    style={{background: '#f9f9f9', display: 'block'}}
                                />
                                <div style={{
                                    position: 'absolute',
                                    top: 10,
                                    left: 10,
                                    background: 'rgba(255,255,255,0.8)',
                                    padding: 5
                                }}>
                                    使用 WASD 或 方向键 移动<br/>
                                    使用 QE 键旋转选中的障碍物<br/>
                                    选中障碍物后按 Delete 键删除
                                </div>
                            </div>

                            <div style={{display: 'flex', gap: '10px', flexWrap: 'wrap', justifyContent: 'center'}}>
                                <button onClick={() => sendCommand('ArrowUp')}>指令: 前进</button>
                                <button onClick={() => sendCommand('ArrowLeft')}>指令: 左转</button>
                                <button onClick={() => sendCommand('ArrowRight')}>指令: 右转</button>
                                <button onClick={() => sendCommand('ArrowDown')}>指令: 后退</button>
                                <button onClick={resetCar}>重置 (Reset)</button>
                        <button onClick={() => setActEnabled(v => !v)}>切换 ACT</button>
                    </div>
                    <div style={{marginTop: 8, fontSize: 12, opacity: 0.8}}>
                        {actStatus}
                            </div>
                        </div>
                        {/* 右侧：第一人称 */}
                        <div style={{position: 'relative', display: 'flex', flexDirection: 'column', alignItems: 'center'}}>
                            <div style={{
                                position: 'absolute',
                                top: 5,
                                left: 5,
                                background: 'rgba(255,255,255,0.7)',
                                padding: '2px 5px',
                                fontSize: '12px'
                            }}>车载摄像头 (Camera)
                            </div>
                            <canvas ref={fpvRef} width={320} height={240}
                                    style={{background: '#000', border: '4px solid #333'}}/>
                            <div style={{marginTop: '10px', fontSize: '14px', color: '#555', width: 320}}>
                                说明：右侧画面是根据左侧地图实时计算生成的伪3D视角。
                            </div>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    )
}

export default SimPage
