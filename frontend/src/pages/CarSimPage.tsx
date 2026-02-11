import {useEffect, useRef} from 'react';

const CarSimulator = () => {
    // 两个 Canvas 的 Ref
    const topDownRef = useRef(null); // 上帝视角
    const fpvRef = useRef(null);     // 第一人称视角

    // 地图尺寸
    const MAP_W = 600;
    const MAP_H = 600;

    // 定义障碍物 (x, y, w, h, color)
    const OBSTACLES = [
        {x: 200, y: 150, w: 100, h: 100, color: '#8e44ad'}, // 紫色墙
        {x: 400, y: 400, w: 50, h: 150, color: '#e67e22'},  // 橙色墙
        {x: 100, y: 400, w: 150, h: 50, color: '#16a085'},  // 绿色墙
        {x: 450, y: 100, w: 50, h: 50, color: '#c0392b'},   // 红色柱子
    ];

    const INITIAL_STATE = {
        x: 300, y: 300, angle: -Math.PI / 2, speed: 0
    };

    const gameState = useRef({
        ...INITIAL_STATE,
        rotationSpeed: 0.06,
        maxSpeed: 4,
        acceleration: 0.1,
        friction: 0.96
    });

    const keys = useRef({});

    useEffect(() => {
        // 获取上下文
        const ctxTop = topDownRef.current.getContext('2d');
        const ctxFpv = fpvRef.current.getContext('2d');

        // 禁用平滑处理，让像素风更清晰（可选）
        ctxFpv.imageSmoothingEnabled = false;

        let animationFrameId;

        const handleKeyDown = (e) => {
            keys.current[e.code] = true;
        };
        const handleKeyUp = (e) => {
            keys.current[e.code] = false;
        };

        window.addEventListener('keydown', handleKeyDown);
        window.addEventListener('keyup', handleKeyUp);

        const loop = () => {
            updatePhysics();
            drawTopDown(ctxTop);
            drawFirstPerson(ctxFpv); // 核心：绘制 FPV
            animationFrameId = window.requestAnimationFrame(loop);
        };

        loop();

        return () => {
            window.removeEventListener('keydown', handleKeyDown);
            window.removeEventListener('keyup', handleKeyUp);
            window.cancelAnimationFrame(animationFrameId);
        };
    }, []);

    const resetCar = () => {
        gameState.current.x = INITIAL_STATE.x;
        gameState.current.y = INITIAL_STATE.y;
        gameState.current.angle = INITIAL_STATE.angle;
        gameState.current.speed = 0;
    };

    // --- 物理引擎 ---
    const updatePhysics = () => {
        const state = gameState.current;

        if (keys.current['ArrowLeft'] || keys.current['KeyA']) state.angle -= state.rotationSpeed;
        if (keys.current['ArrowRight'] || keys.current['KeyD']) state.angle += state.rotationSpeed;
        if (keys.current['ArrowUp'] || keys.current['KeyW']) state.speed += state.acceleration;
        if (keys.current['ArrowDown'] || keys.current['KeyS']) state.speed -= state.acceleration;

        // 限制速度
        if (state.speed > state.maxSpeed) state.speed = state.maxSpeed;
        if (state.speed < -state.maxSpeed / 2) state.speed = -state.maxSpeed / 2;

        state.speed *= state.friction;

        const nextX = state.x + Math.cos(state.angle) * state.speed;
        const nextY = state.y + Math.sin(state.angle) * state.speed;

        // 简单的碰撞检测 (如果不撞墙才移动)
        if (!checkCollision(nextX, nextY)) {
            state.x = nextX;
            state.y = nextY;
        } else {
            state.speed = 0; // 撞墙停下
        }
    };

    // 检查点是否在任何障碍物内
    const checkCollision = (x, y) => {
        // 边界检查
        if (x < 0 || x > MAP_W || y < 0 || y > MAP_H) return true;
        // 障碍物检查
        return OBSTACLES.some(obs =>
            x > obs.x && x < obs.x + obs.w &&
            y > obs.y && y < obs.y + obs.h
        );
    };

    // --- 渲染：上帝视角 ---
    const drawTopDown = (ctx) => {
        ctx.clearRect(0, 0, MAP_W, MAP_H);

        // 1. 画地板网格
        ctx.strokeStyle = '#eee';
        ctx.lineWidth = 1;
        ctx.beginPath();
        for (let i = 0; i <= MAP_W; i += 50) {
            ctx.moveTo(i, 0);
            ctx.lineTo(i, MAP_H);
        }
        for (let i = 0; i <= MAP_H; i += 50) {
            ctx.moveTo(0, i);
            ctx.lineTo(MAP_W, i);
        }
        ctx.stroke();

        // 2. 画障碍物
        OBSTACLES.forEach(obs => {
            ctx.fillStyle = obs.color;
            ctx.fillRect(obs.x, obs.y, obs.w, obs.h);
            ctx.strokeStyle = '#333';
            ctx.strokeRect(obs.x, obs.y, obs.w, obs.h);
        });

        // 3. 画小车
        const {x, y, angle} = gameState.current;
        ctx.save();
        ctx.translate(x, y);
        ctx.rotate(angle);
        ctx.fillStyle = 'blue';
        ctx.fillRect(-10, -5, 20, 10);
        ctx.fillStyle = 'yellow'; // 车灯
        ctx.beginPath();
        ctx.arc(10, -3, 2, 0, Math.PI * 2);
        ctx.arc(10, 3, 2, 0, Math.PI * 2);
        ctx.fill();
        ctx.restore();

        // 4. (可选) 画出视野范围示意线，让你理解原理
        ctx.strokeStyle = 'rgba(0,0,0,0.1)';
        ctx.beginPath();
        ctx.moveTo(x, y);
        ctx.lineTo(x + Math.cos(angle - Math.PI / 6) * 100, y + Math.sin(angle - Math.PI / 6) * 100);
        ctx.moveTo(x, y);
        ctx.lineTo(x + Math.cos(angle + Math.PI / 6) * 100, y + Math.sin(angle + Math.PI / 6) * 100);
        ctx.stroke();
    };

    // --- 核心算法：光线投射 (Raycasting) ---
    const drawFirstPerson = (ctx) => {
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
            const rayAngle = (angle - fov / 2) + (i / rayCount) * fov;

            // 计算这一条射线碰到了什么，以及距离是多少
            const hit = castRay(x, y, rayAngle);

            if (hit) {
                // 修正鱼眼效应 (核心步骤：如果不乘 cos，墙壁会看起来弯曲)
                const correctedDist = hit.distance * Math.cos(rayAngle - angle);

                // 计算墙在屏幕上的高度 (距离越近，墙越高)
                const wallHeight = (h * 40) / correctedDist;

                // 绘制墙体垂直线条
                ctx.fillStyle = hit.color;
                // 根据距离加一点阴影 (越远越暗)
                ctx.globalAlpha = Math.max(0.3, 1 - correctedDist / 600);
                ctx.fillRect(i * rayWidth, (h - wallHeight) / 2, rayWidth + 1, wallHeight);
                ctx.globalAlpha = 1.0;
            }
        }
    };

    // 发射单条射线，寻找最近的交点
    const castRay = (sx, sy, angle) => {
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

    // 数学公式：射线与线段相交检测
    const getRaySegmentIntersection = (rx, ry, rdx, rdy, wall) => {
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

    return (
        <div style={{
            display: 'flex',
            flexDirection: 'column',
            alignItems: 'center',
            gap: '20px',
            fontFamily: 'sans-serif'
        }}>
            <h2>双视角小车模拟器</h2>

            <div style={{display: 'flex', gap: '20px', alignItems: 'flex-start'}}>
                {/* 左侧：上帝视角 */}
                <div style={{position: 'relative'}}>
                    <div style={{
                        position: 'absolute',
                        top: 5,
                        left: 5,
                        background: 'rgba(255,255,255,0.7)',
                        padding: '2px 5px',
                        fontSize: '12px'
                    }}>俯视图 (Top-Down)
                    </div>
                    <canvas ref={topDownRef} width={MAP_W} height={MAP_H}
                            style={{background: '#f9f9f9', border: '2px solid #333'}}/>
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

            <div style={{display: 'flex', gap: '10px'}}>
                <button onClick={resetCar} style={{
                    padding: '8px 16px',
                    background: '#e74c3c',
                    color: 'white',
                    border: 'none',
                    borderRadius: '4px'
                }}>重置位置
                </button>
                <span style={{lineHeight: '30px', color: '#666'}}>使用 WASD 或 方向键移动</span>
            </div>
        </div>
    );
};

export default CarSimulator;