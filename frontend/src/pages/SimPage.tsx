import {useEffect, useRef} from "react"

const INITIAL_STATE = {
    x: 400,          // 初始 X 坐标
    y: 300,          // 初始 Y 坐标
    angle: -Math.PI / 2, // 初始角度 (弧度)，-PI/2 朝上
    speed: 0,        // 当前速度
}

// 定义障碍物 (x, y, w, h, color)
const OBSTACLES = [
    {x: 200, y: 150, w: 100, h: 100, color: '#8e44ad'}, // 紫色墙
    {x: 400, y: 400, w: 50, h: 150, color: '#e67e22'},  // 橙色墙
    {x: 100, y: 400, w: 150, h: 50, color: '#16a085'},  // 绿色墙
    {x: 450, y: 100, w: 50, h: 50, color: '#c0392b'},   // 红色柱子
];

// 地图尺寸
const MAP_W = 800;
const MAP_H = 600;

const SimPage = () => {
    const canvasRef = useRef<HTMLCanvasElement | null>(null)
    const fpvRef = useRef<HTMLCanvasElement | null>(null);     // 第一人称视角
    const gameState = useRef({
        ...INITIAL_STATE,
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
        if (checkCollision(state.x, state.y)) {
            state.x -= Math.cos(state.angle) * state.speed * 2
            state.y -= Math.sin(state.angle) * state.speed * 2
            state.speed = 0
        }
    }

    // 检查点是否在任何障碍物内
    const checkCollision = (x: number, y: number) => {
        // 边界检查
        if (x < 0 || x > MAP_W || y < 0 || y > MAP_H) return true;
        // 障碍物检查
        return OBSTACLES.some(obs =>
            x > obs.x && x < obs.x + obs.w &&
            y > obs.y && y < obs.y + obs.h
        );
    };

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
        const {x, y, angle} = gameState.current;
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

    // --- 绘图逻辑 ---
    const drawTopDown = (ctx: CanvasRenderingContext2D) => {
        // 清空画布
        ctx.clearRect(0, 0, ctx.canvas.width, ctx.canvas.height)

        // 绘制背景网格 (模拟地面)
        drawGrid(ctx, ctx.canvas.width, ctx.canvas.height)

        // 保存当前绘图状态
        ctx.save()

        // 2. 画障碍物
        OBSTACLES.forEach(obs => {
            ctx.fillStyle = obs.color;
            ctx.fillRect(obs.x, obs.y, obs.w, obs.h);
            ctx.strokeStyle = '#333';
            ctx.strokeRect(obs.x, obs.y, obs.w, obs.h);
        });

        // 3. 绘制小车 (此时原点就是车身中心)
        drawCarBody(ctx)

        const {x, y, angle} = gameState.current;
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

    // 数学公式：射线与线段相交检测
    const getRaySegmentIntersection = (rx: number, ry:number, rdx: number, rdy: number, wall: {x1: number, y1: number, x2: number, y2: number}) => {
        const {x1, y1, x2, y2} = wall;
        const v1x = x1 - rx;
        const v1y = y1 - ry;
        const v2x = x2 - x1;
        const v2y = y2 - y1;
        const v3x = -rdx; // 射线方向反转
        const v3y = -rdy;

        const cross = v2x * v3y - v2y * v3x;
        if (Math.abs(cross) < 0.0001) return null; // 平行

        const t1 = (v2x * v1y - v2y * v1x) / cross; // 射线距离
        const t2 = (v3x * v1y - v3y * v1x) / cross; // 线段比例 (0~1)

        // t1 > 0 代表射线前方，t2 在 0~1 代表交点在线段上
        if (t1 > 0 && t2 >= 0 && t2 <= 1) {
            return t1;
        }
        return null;
    };

    // 发射单条射线，寻找最近的交点
    const castRay = (sx: number, sy: number, angle: number) => {
        const cos = Math.cos(angle);
        const sin = Math.sin(angle);
        let minDist = Infinity;
        let hitColor = null;

        // 将所有障碍物转换为线段进行检测
        const boundaries = [
            {x1: 0, y1: 0, x2: MAP_W, y2: 0, color: '#333'}, // 上墙
            {x1: MAP_W, y1: 0, x2: MAP_W, y2: MAP_H, color: '#333'}, // 右墙
            {x1: MAP_W, y1: MAP_H, x2: 0, y2: MAP_H, color: '#333'}, // 下墙
            {x1: 0, y1: MAP_H, x2: 0, y2: 0, color: '#333'}  // 左墙
        ];

        // 把矩形障碍物拆成4条线段
        OBSTACLES.forEach(obs => {
            const c = obs.color;
            boundaries.push({x1: obs.x, y1: obs.y, x2: obs.x + obs.w, y2: obs.y, color: c});
            boundaries.push({x1: obs.x + obs.w, y1: obs.y, x2: obs.x + obs.w, y2: obs.y + obs.h, color: c});
            boundaries.push({x1: obs.x + obs.w, y1: obs.y + obs.h, x2: obs.x, y2: obs.y + obs.h, color: c});
            boundaries.push({x1: obs.x, y1: obs.y + obs.h, x2: obs.x, y2: obs.y, color: c});
        });

        // 检测射线与每一条线段的交点
        boundaries.forEach(wall => {
            const dist = getRaySegmentIntersection(sx, sy, cos, sin, wall);
            if (dist !== null && dist < minDist) {
                minDist = dist;
                hitColor = wall.color;
            }
        });

        return minDist === Infinity ? null : {distance: minDist, color: hitColor};
    };

    const drawFirstPerson = (ctx: CanvasRenderingContext2D) => {
        const w = ctx.canvas.width;
        const h = ctx.canvas.height;
        const {x, y, angle} = gameState.current;

        // 天空和地面
        ctx.fillStyle = '#87CEEB'; // 天空蓝
        ctx.fillRect(0, 0, w, h / 2);
        ctx.fillStyle = '#7f8c8d'; // 地面灰
        ctx.fillRect(0, h / 2, w, h / 2);

        // 参数
        const fov = Math.PI / 3; // 60度视野
        const rayCount = w / 4;  // 射线数量 (为了性能，每4个像素投射一条，然后画宽一点)
        const rayWidth = w / rayCount;

        // 遍历每一条射线
        for (let i = 0; i < rayCount; i++) {
            // 当前射线角度 = 车角度 - 半个FOV + 增量
            const rayAngle = (angle + Math.PI - fov / 2) + (i / rayCount) * fov;

            // 计算这一条射线碰到了什么，以及距离是多少
            const hit = castRay(x, y, rayAngle);

            if (hit) {
                // 修正鱼眼效应 (核心步骤：如果不乘 cos，墙壁会看起来弯曲)
                const correctedDist = hit.distance * Math.cos(rayAngle - angle);

                // 计算墙在屏幕上的高度 (距离越近，墙越高)
                const wallHeight = (h * 40) / correctedDist;

                // 绘制墙体垂直线条
                // eslint-disable-next-line @typescript-eslint/ban-ts-comment
                // @ts-expect-error
                ctx.fillStyle = hit.color;
                // 根据距离加一点阴影 (越远越暗)
                ctx.globalAlpha = Math.max(0.3, 1 - correctedDist / 600);
                ctx.fillRect(i * rayWidth, (h - wallHeight) / 2, rayWidth + 1, wallHeight);
                ctx.globalAlpha = 1.0;
            }
        }
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

        // 2. 核心渲染循环
        const renderLoop = () => {
            updatePhysics()
            drawTopDown(ctxTop)
            drawFirstPerson(ctxFpv)
            animationFrameId = window.requestAnimationFrame(renderLoop)
        }

        renderLoop()

        // 清理函数
        return () => {
            window.removeEventListener('keydown', handleKeyDown)
            window.removeEventListener('keyup', handleKeyUp)
            window.cancelAnimationFrame(animationFrameId)
        }
    }, [drawTopDown, drawFirstPerson])

    // --- 外部指令模拟 ---
    const sendCommand = (cmd: string) => {
        // 模拟指令只需修改 keys ref 的状态即可
        keys.current[cmd] = true
        setTimeout(() => {
            keys.current[cmd] = false
        }, 200) // 模拟按键按下200毫秒
    }

    const resetCar = () => {
        gameState.current.x = INITIAL_STATE.x
        gameState.current.y = INITIAL_STATE.y
        gameState.current.angle = INITIAL_STATE.angle
        gameState.current.speed = INITIAL_STATE.speed
    }

    return (
        <div style={{ display: 'flex', flexDirection: 'column', gap: '20px', justifyContent: 'space-around', alignItems: 'center' }}>
            <h2>小车模拟器</h2>
            <div style={{ display: 'flex', flexDirection: 'row', gap: '20px' }}>
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
                {/* 右侧：第一人称 */}
                <div style={{position: 'relative'}}>
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
    )
}

export default SimPage