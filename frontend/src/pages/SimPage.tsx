import {useEffect, useRef} from "react"

const START_STATE = {
    x: 400,          // 初始 X 坐标
    y: 300,          // 初始 Y 坐标
    angle: -Math.PI / 2, // 初始角度 (弧度)，-PI/2 朝上
    speed: 0,        // 当前速度
}

const SimPage = () => {
    const canvasRef = useRef<HTMLCanvasElement | null>(null)
    const gameState = useRef({
        ...START_STATE,
        maxSpeed: 5,     // 最大速度
        acceleration: 0.2, // 加速度
        friction: 0.95,  // 摩擦力 (模拟惯性)
        rotationSpeed: 0.05 // 转向灵敏度
    })

    const keys = useRef<Record<string, boolean>>({})

    // --- 物理计算逻辑 ---
    const updatePhysics = () => {
        const state = gameState.current

        // 前进 / 后退
        if (keys.current['ArrowUp'] || keys.current['KeyW']) {
            if (state.speed < state.maxSpeed) state.speed += state.acceleration
        }
        if (keys.current['ArrowDown'] || keys.current['KeyS']) {
            if (state.speed > -state.maxSpeed / 2) state.speed -= state.acceleration
        }

        if (keys.current['ArrowLeft'] || keys.current['KeyA']) {
            state.angle -= state.rotationSpeed
        }
        if (keys.current['ArrowRight'] || keys.current['KeyD']) {
            state.angle += state.rotationSpeed
        }

        // 应用摩擦力 (自然减速)
        state.speed *= state.friction

        // 更新坐标 (核心三角函数：x = v*cos(θ), y = v*sin(θ))
        state.x += Math.cos(state.angle) * state.speed
        state.y += Math.sin(state.angle) * state.speed

        // 简单的边界检测 (碰到墙壁反弹)
        if (state.x < 0 || state.x > 800) {
            state.x -= Math.cos(state.angle) * state.speed * 2
            state.speed = 0
        }
        if (state.y < 0 || state.y > 600) {
            state.y -= Math.sin(state.angle) * state.speed * 2
            state.speed = 0
        }
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
        // 车身 (长方形)
        ctx.fillStyle = '#3498db'
        // fillRect(x, y, width, height) - x,y 偏移宽高的一半以保持中心旋转
        ctx.fillRect(-20, -10, 40, 20)

        // 车灯 (黄色，表示车头方向)
        ctx.fillStyle = '#f1c40f'
        ctx.beginPath()
        ctx.arc(15, -6, 3, 0, Math.PI * 2) // 右前灯
        ctx.arc(15, 6, 3, 0, Math.PI * 2)  // 左前灯
        ctx.fill()

        // 挡风玻璃
        ctx.fillStyle = '#2c3e50'
        ctx.fillRect(5, -8, 10, 16)
    }

    // --- 绘图逻辑 ---
    const draw = (ctx: CanvasRenderingContext2D, canvas: HTMLCanvasElement) => {
        const state = gameState.current

        // 清空画布
        ctx.clearRect(0, 0, canvas.width, canvas.height)

        // 绘制背景网格 (模拟地面)
        drawGrid(ctx, canvas.width, canvas.height)

        // 保存当前绘图状态
        ctx.save()

        // 1. 移动画布原点到小车中心
        ctx.translate(state.x, state.y)
        // 2. 旋转画布
        ctx.rotate(state.angle)

        // 3. 绘制小车 (此时原点就是车身中心)
        drawCarBody(ctx)

        // 恢复绘图状态
        ctx.restore()
    }


    useEffect(() => {
        const canvas = canvasRef.current
        if (canvas == null) return
        const ctx = canvas.getContext('2d')
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

        // 2. 核心渲染循环
        const renderLoop = () => {
            updatePhysics()
            if (ctx == null) return
            draw(ctx, canvas)
            animationFrameId = window.requestAnimationFrame(renderLoop)
        }

        renderLoop()

        // 清理函数
        return () => {
            window.removeEventListener('keydown', handleKeyDown)
            window.removeEventListener('keyup', handleKeyUp)
            window.cancelAnimationFrame(animationFrameId)
        }
    }, [])

    // --- 外部指令模拟 ---
    const sendCommand = (cmd: string) => {
        // 模拟指令只需修改 keys ref 的状态即可
        keys.current[cmd] = true
        setTimeout(() => {
            keys.current[cmd] = false
        }, 200) // 模拟按键按下200毫秒
    }

    const resetCar = () => {
        gameState.current.x = START_STATE.x
        gameState.current.y = START_STATE.y
        gameState.current.angle = START_STATE.angle
        gameState.current.speed = START_STATE.speed
    }

    return (
        <div style={{display: 'flex', flexDirection: 'column', alignItems: 'center', gap: '20px'}}>
            <h2>俯视小车模拟器</h2>
            <div style={{position: 'relative', border: '2px solid #333'}}>
                <canvas
                    ref={canvasRef}
                    width={800}
                    height={600}
                    style={{background: '#f9f9f9', display: 'block'}}
                />
                <div style={{position: 'absolute', top: 10, left: 10, background: 'rgba(255,255,255,0.8)', padding: 5}}>
                    使用 WASD 或 方向键 移动
                </div>
            </div>

            <div style={{display: 'flex', gap: '10px'}}>
                <button onClick={() => sendCommand('ArrowUp')}>指令: 前进</button>
                <button onClick={() => sendCommand('ArrowLeft')}>指令: 左转</button>
                <button onClick={() => sendCommand('ArrowRight')}>指令: 右转</button>
                <button onClick={() => sendCommand('ArrowDown')}>指令: 后退</button>
                <button onClick={resetCar}>重置 (Reset)</button>
            </div>
        </div>
    )
}

export default SimPage