import {useEffect, useRef, useState} from "react"
import {getCarState, resetCar, sendAction, socket} from "../api/socket";
import type {Car} from "../model/car";
import {useObstacleStore, type Obstacle} from "../store/obstacleStore";

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
    const { obstacles, setObstacles, updateObstacle, removeObstacle } = useObstacleStore();
    
    // 编辑状态管理
    const [editingObstacle, setEditingObstacle] = useState<Obstacle | null>(null);
    const [editForm, setEditForm] = useState<Partial<Obstacle>>({});

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
    
    // 选中状态管理
    const [selectedObstacleId, setSelectedObstacleId] = useState<string | null>(null);
    
    // 障碍物创建状态管理
    const [selectedObstacleType, setSelectedObstacleType] = useState<'RECT' | 'CIRCLE'>('RECT');
    const [isCreatingObstacle, setIsCreatingObstacle] = useState(false);

    // ----------------------------------------
    // 状态管理：小车状态 (使用 ref 避免频繁重渲染)
    // ----------------------------------------
    const carState = useRef({
        x: 400,          // 初始 X 坐标
        y: 300,          // 初始 Y 坐标
        angle: -Math.PI / 2, // 初始角度 (弧度)，-PI/2 朝上
    })

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

    const keys = useRef<Record<string, boolean>>({})

    // --- 物理计算逻辑 ---
    const updatePhysics = () => {

        
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
                setSelectedObstacleId(null);
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
        return obstaclesRef.current.some(obs => {
            if (obs.type === 'RECT') {
                const width = obs.w || 0;
                const height = obs.h || 0;
                
                // 如果障碍物没有旋转，使用简单的轴对齐碰撞检测
                if (!obs.angle) {
                    return x > obs.x && x < obs.x + width &&
                           y > obs.y && y < obs.y + height;
                }
                
                // 如果障碍物有旋转，使用旋转碰撞检测
                const centerX = obs.x + width / 2;
                const centerY = obs.y + height / 2;
                
                // 将点相对于矩形中心点进行旋转
                const dx = x - centerX;
                const dy = y - centerY;
                const rotatedX = dx * Math.cos(-obs.angle) - dy * Math.sin(-obs.angle);
                const rotatedY = dx * Math.sin(-obs.angle) + dy * Math.cos(-obs.angle);
                
                // 检查旋转后的点是否在轴对齐的矩形内
                return Math.abs(rotatedX) < width / 2 && Math.abs(rotatedY) < height / 2;
            } else if (obs.type === 'CIRCLE') {
                // 圆形碰撞检测
                const dx = x - obs.x;
                const dy = y - obs.y;
                const distance = Math.sqrt(dx * dx + dy * dy);
                return distance < (obs.r || 0);
            }
            return false;
        });
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

        // 2. 画障碍物：遍历障碍物数组，使用 obstaclesRef.current 获取最新数据
        obstaclesRef.current.forEach(obs => {
            ctx.fillStyle = obs.color;
            if (obs.type === 'RECT') {
                const width = obs.w || 0;
                const height = obs.h || 0;
                const centerX = obs.x + width / 2;
                const centerY = obs.y + height / 2;
                
                // 保存当前状态
                ctx.save();
                
                // 平移到中心点并旋转
                if (obs.angle) {
                    ctx.translate(centerX, centerY);
                    ctx.rotate(obs.angle);
                    ctx.translate(-centerX, -centerY);
                }
                
                // 绘制矩形障碍物
                ctx.fillRect(obs.x, obs.y, width, height);
                ctx.strokeStyle = '#333';
                ctx.strokeRect(obs.x, obs.y, width, height);
                
                // 为选中的障碍物添加高亮效果
                if (obs.id === selectedObstacleId) {
                    ctx.strokeStyle = '#ff0000';
                    ctx.lineWidth = 2;
                    ctx.setLineDash([5, 5]);
                    ctx.strokeRect(obs.x - 5, obs.y - 5, width + 10, height + 10);
                    ctx.setLineDash([]);
                    ctx.lineWidth = 1;
                }
                
                // 恢复状态
                ctx.restore();
            } else if (obs.type === 'CIRCLE') {
                // 绘制圆形障碍物
                ctx.beginPath();
                ctx.arc(obs.x, obs.y, obs.r || 0, 0, Math.PI * 2);
                ctx.fill();
                ctx.strokeStyle = '#333';
                ctx.stroke();
                
                // 为选中的障碍物添加高亮效果
                if (obs.id === selectedObstacleId) {
                    ctx.strokeStyle = '#ff0000';
                    ctx.lineWidth = 2;
                    ctx.setLineDash([5, 5]);
                    ctx.beginPath();
                    ctx.arc(obs.x, obs.y, (obs.r || 0) + 5, 0, Math.PI * 2);
                    ctx.stroke();
                    ctx.setLineDash([]);
                    ctx.lineWidth = 1;
                }
            }
        });

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

    // 数学公式：射线与线段相交检测
    const getRaySegmentIntersection = (rx: number, ry: number, rdx: number, rdy: number, wall: {
        x1: number,
        y1: number,
        x2: number,
        y2: number
    }) => {
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

    // 检查鼠标点击是否在障碍物内
    const getObstacleAtPosition = (x: number, y: number) => {
        return obstaclesRef.current.find(obs => {
            if (obs.type === 'RECT') {
                return x >= obs.x && x <= obs.x + (obs.w || 0) &&
                       y >= obs.y && y <= obs.y + (obs.h || 0);
            } else if (obs.type === 'CIRCLE') {
                const dx = x - obs.x;
                const dy = y - obs.y;
                const distance = Math.sqrt(dx * dx + dy * dy);
                return distance <= (obs.r || 0);
            }
            return false;
        });
    };
    
    // 创建新障碍物
    const createObstacle = (x: number, y: number) => {
        const newObstacle = {
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
        
        // 添加新障碍物到状态
        setObstacles([...obstacles, newObstacle]);
    };

    // 在摄像头前方创建障碍物
    const createObstacleInFrontOfCamera = () => {
        const {x, y, angle} = carState.current;
        // 计算摄像头前方50像素的位置
        const frontX = x + Math.cos(angle) * 50;
        const frontY = y + Math.sin(angle) * 50;
        createObstacle(frontX, frontY);
    };

    // ============================================================
    // 射线投射函数 - 用于第一人称视角渲染
    // 参数：
    //   sx, sy - 射线起点坐标（小车位置）
    //   angle - 射线发射角度
    // 返回：{distance: number, color: string} - 最近的交点距离和颜色
    // ============================================================
    const castRay = (sx: number, sy: number, angle: number) => {
        const cos = Math.cos(angle);
        const sin = Math.sin(angle);
        let minDist = Infinity;
        let hitColor = null;

        // 将所有障碍物转换为线段进行检测
        // 首先添加地图四周的边界墙
        const boundaries = [
            {x1: 0, y1: 0, x2: MAP_W, y2: 0, color: '#333'}, // 上墙
            {x1: MAP_W, y1: 0, x2: MAP_W, y2: MAP_H, color: '#333'}, // 右墙
            {x1: MAP_W, y1: MAP_H, x2: 0, y2: MAP_H, color: '#333'}, // 下墙
            {x1: 0, y1: MAP_H, x2: 0, y2: 0, color: '#333'}  // 左墙
        ];

        // 处理障碍物
        // 使用 obstaclesRef.current 获取最新的障碍物数据
        obstaclesRef.current.forEach(obs => {
            const c = obs.color;
            if (obs.type === 'RECT') {
                const width = obs.w || 0;
                const height = obs.h || 0;
                
                // 定义矩形的四个顶点
                let vertices = [
                    {x: obs.x, y: obs.y},              // 左上角
                    {x: obs.x + width, y: obs.y},       // 右上角
                    {x: obs.x + width, y: obs.y + height}, // 右下角
                    {x: obs.x, y: obs.y + height}       // 左下角
                ];
                
                // 如果障碍物有旋转，计算旋转后的顶点
                if (obs.angle) {
                    const centerX = obs.x + width / 2;
                    const centerY = obs.y + height / 2;
                    
                    vertices = vertices.map(vertex => {
                        // 相对于中心点的坐标
                        const dx = vertex.x - centerX;
                        const dy = vertex.y - centerY;
                        
                        // 旋转坐标
                        const angle = obs.angle || 0;
                        const rotatedX = dx * Math.cos(angle) - dy * Math.sin(angle);
                        const rotatedY = dx * Math.sin(angle) + dy * Math.cos(angle);
                        
                        // 转换回绝对坐标
                        return {
                            x: rotatedX + centerX,
                            y: rotatedY + centerY
                        };
                    });
                }
                
                // 把矩形障碍物拆成4条线段
                // 上边
                boundaries.push({x1: vertices[0].x, y1: vertices[0].y, x2: vertices[1].x, y2: vertices[1].y, color: c});
                // 右边
                boundaries.push({x1: vertices[1].x, y1: vertices[1].y, x2: vertices[2].x, y2: vertices[2].y, color: c});
                // 下边
                boundaries.push({x1: vertices[2].x, y1: vertices[2].y, x2: vertices[3].x, y2: vertices[3].y, color: c});
                // 左边
                boundaries.push({x1: vertices[3].x, y1: vertices[3].y, x2: vertices[0].x, y2: vertices[0].y, color: c});
            }
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

    // ============================================================
    // 精灵渲染函数 - 用于渲染圆形障碍物
    // 参数：
    //   ctx - Canvas 2D 绘图上下文
    //   carX, carY, carAngle - 小车位置和角度
    // ============================================================


    // 帧率控制变量
    const lastPrintTime = useRef(0);

    const drawFirstPerson = (ctx: CanvasRenderingContext2D) => {
        const w = ctx.canvas.width;
        const h = ctx.canvas.height;
        const {x, y, angle} = carState.current;
        const currentTime = Date.now();

        // 天空和地面
        ctx.fillStyle = '#87CEEB'; // 天空蓝
        ctx.fillRect(0, 0, w, h / 2);
        ctx.fillStyle = '#7f8c8d'; // 地面灰
        ctx.fillRect(0, h / 2, w, h / 2);

        // 参数
        const fov = Math.PI / 3; // 60度视野
        const rayCount = w / 4;  // 射线数量 (为了性能，每4个像素投射一条，然后画宽一点)
        const rayWidth = w / rayCount;

        // 初始化深度缓冲
        const depthBuffer = new Array(rayCount).fill(Infinity);

        // 控制打印频率：每秒只打印一次
        const shouldPrint = currentTime - lastPrintTime.current > 1000;

        // 遍历每一条射线
        for (let i = 0; i < rayCount; i++) {
            // 当前射线角度 = 车角度 + PI - 半个FOV + 增量（修正为车头视角）
            const rayAngle = (angle + Math.PI - fov / 2) + (i / rayCount) * fov;

            // 计算这一条射线碰到了什么，以及距离是多少
            const hit = castRay(x, y, rayAngle);

            if (hit) {
                // 修正鱼眼效应 (核心步骤：如果不乘 cos，墙壁会看起来弯曲)
                const correctedDist = hit.distance * Math.cos(rayAngle - angle);

                // 计算墙在屏幕上的高度 (距离越近，墙越高)
                const wallHeight = (h * 40) / correctedDist;

                // 把这一列的墙体深度存入 depthBuffer（确保为正数）
                depthBuffer[i] = Math.abs(correctedDist);

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

        // 准备网球精灵数据
        const balls = obstaclesRef.current.filter(obs => obs.type === 'CIRCLE');
        if (shouldPrint) {
            console.log('圆形障碍物数量:', balls.length);
            console.log('圆形障碍物数据:', balls);
        }

        // 把物理数据转换成渲染数据
        const sprites = balls.map(ball => {
            // 1. 计算相对坐标和距离
            const dx = ball.x - x;
            const dy = ball.y - y;
            const dist = Math.sqrt(dx * dx + dy * dy);

            // 2. 计算相对角度
            let spriteAngle = Math.atan2(dy, dx) - angle;
            
            // 角度归一化：把角度限制在 [-PI, PI] 之间
            while (spriteAngle > Math.PI) spriteAngle -= Math.PI * 2;
            while (spriteAngle < -Math.PI) spriteAngle += Math.PI * 2;

            // 3. 鱼眼修正
            const correctedDist = Math.abs(dist * Math.cos(spriteAngle));

            // 4. 计算屏幕 X 坐标
            const screenX = (w / 2) + (spriteAngle / fov) * w;

            // 5. 计算精灵尺寸（套用矩形障碍物的高度计算逻辑）
            const spriteSize = (h * 40) / correctedDist * ((ball.r || 15) / 15);

            // 6. 屏幕 Y 坐标：以下界为准，与矩形障碍物对齐
            // 计算矩形障碍物在相同距离的底部位置
            const wallHeight = (h * 40) / correctedDist;
            const wallBottom = h / 2 + wallHeight / 2;
            // 计算球体的底部位置（与矩形对齐）
            const screenY = wallBottom - spriteSize / 2;

            return {
                screenX,
                screenY,
                size: spriteSize,
                dist: correctedDist,
                realDist: dist,
                color: ball.color,
                isPicked: false
            };
        })
        // 按原始距离从远到近排序
        .sort((a, b) => b.realDist - a.realDist);

        if (shouldPrint) {
            console.log('精灵数据:', sprites);
            lastPrintTime.current = currentTime;
        }

        // 列裁剪渲染精灵
        sprites.forEach((sprite, index) => {
            if (shouldPrint) {
                console.log(`精灵 ${index} 数据:`, {
                    screenX: sprite.screenX,
                    screenY: sprite.screenY,
                    size: sprite.size,
                    dist: sprite.dist,
                    realDist: sprite.realDist,
                    color: sprite.color
                });
            }
            
            // 基础边界检查
            if (sprite.dist < 1 || sprite.realDist > 1000) {
                if (shouldPrint) {
                    console.log(`精灵 ${index} 被边界检查过滤:`, { dist: sprite.dist, realDist: sprite.realDist });
                }
                return;
            }

            // 计算精灵覆盖的射线列索引范围
            const leftCol = Math.max(0, Math.floor((sprite.screenX - sprite.size / 2) / rayWidth));
            const rightCol = Math.min(rayCount - 1, Math.ceil((sprite.screenX + sprite.size / 2) / rayWidth));
            
            if (shouldPrint) {
                console.log(`精灵 ${index} 列范围:`, { leftCol, rightCol, rayCount });
            }

            // 遍历精灵覆盖的每一列
            for (let i = leftCol; i <= rightCol; i++) {
                // 核心遮挡判断逻辑
                if (sprite.dist >= depthBuffer[i]) {
                    if (shouldPrint) {
                        console.log(`精灵 ${index} 列 ${i} 被墙挡住:`, { spriteDist: sprite.dist, wallDist: depthBuffer[i] });
                    }
                    continue; // 被墙挡住了，下一列
                }

                // 计算这一列在屏幕上的 X 坐标
                const colScreenX = i * rayWidth;

                // 计算这一列在圆上的相对 X 位置 (-1 到 1 之间)
                const relX = (colScreenX + rayWidth / 2 - sprite.screenX) / (sprite.size / 2);

                // 用圆的方程计算这一列的高度
                if (Math.abs(relX) > 1) {
                    if (shouldPrint) {
                        console.log(`精灵 ${index} 列 ${i} 超出圆范围:`, { relX });
                    }
                    continue; // 超出圆的范围，不画
                }
                
                const relY = Math.sqrt(1 - relX * relX);
                const colHeight = relY * sprite.size;
                const colTop = sprite.screenY - colHeight / 2;
                
                if (shouldPrint) {
                    console.log(`精灵 ${index} 列 ${i} 绘制:`, { colScreenX, colTop, colHeight });
                }

                // 绘制这一列的竖线
                ctx.save();
                
                // 基础样式（实色，无透明度）
                ctx.fillStyle = sprite.color;
                ctx.globalAlpha = 1.0;

                // 如果是被夹取状态，加个绿色发光特效
                if (sprite.isPicked) {
                    ctx.shadowColor = '#00ff00';
                    ctx.shadowBlur = 15;
                }

                // 画这一列的竖线
                ctx.fillRect(colScreenX, colTop, rayWidth + 1, colHeight);

                ctx.restore();
            }
        });
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
                            setSelectedObstacleId(clickedObstacle.id);
                            draggingRef.current = {
                                isDragging: true,
                                obstacleId: clickedObstacle.id
                            };
                        } else {
                            setSelectedObstacleId(null);
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
    }, [drawTopDown, drawFirstPerson, updatePhysics, createObstacle, isCreatingObstacle, updateObstacle])

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
                <div style={{
                    flex: '0 0 20%', // 固定占20%宽度，不收缩不增长
                    minWidth: '250px', // 最小宽度，确保内容可读
                    border: '2px solid #333',
                    borderRadius: '8px',
                    padding: '15px',
                    background: '#f9f9f9',
                    overflowY: 'auto',
                    maxHeight: '570px'
                }}>
                    <h3 style={{marginTop: 0, marginBottom: '15px'}}>障碍物管理</h3>
                    
                    {/* 已有障碍物列表 */}
                    <div style={{marginTop: '15px'}}>
                        <h4 style={{marginTop: 0, marginBottom: '10px', fontSize: '14px'}}>已有障碍物列表</h4>
                        <div style={{
                            border: '1px solid rgb(221, 221, 221)',
                            borderRadius: '4px',
                            padding: '8px',
                            maxHeight: '300px',
                            overflowY: 'auto',
                            background: 'rgb(255, 255, 255)'
                        }}>
                            {editingObstacle ? (
                                <div style={{
                                    padding: '12px',
                                    border: '1px solid #4ecdc4',
                                    borderRadius: '4px',
                                    background: 'rgb(255, 255, 255)',
                                    boxShadow: '0 2px 4px rgba(0,0,0,0.1)',
                                    marginBottom: '10px'
                                }}>
                                    <h5 style={{marginTop: 0, marginBottom: '10px', color: '#4ecdc4'}}>编辑障碍物</h5>
                                    <div style={{display: 'flex', flexDirection: 'column', gap: '8px', marginTop: '10px'}}>
                                        <div>
                                            <label style={{fontSize: '12px', marginRight: '5px'}}>X坐标: </label>
                                            <input 
                                                type="number" 
                                                value={editForm.x} 
                                                onChange={(e) => setEditForm({...editForm, x: parseInt(e.target.value)})} 
                                                style={{fontSize: '12px', padding: '3px 5px', width: '80px'}}
                                            />
                                        </div>
                                        <div>
                                            <label style={{fontSize: '12px', marginRight: '5px'}}>Y坐标: </label>
                                            <input 
                                                type="number" 
                                                value={editForm.y} 
                                                onChange={(e) => setEditForm({...editForm, y: parseInt(e.target.value)})} 
                                                style={{fontSize: '12px', padding: '3px 5px', width: '80px'}}
                                            />
                                        </div>
                                        {editingObstacle.type === 'RECT' && (
                                            <>
                                                <div>
                                                    <label style={{fontSize: '12px', marginRight: '5px'}}>宽度: </label>
                                                    <input 
                                                        type="number" 
                                                        value={editForm.w} 
                                                        onChange={(e) => setEditForm({...editForm, w: parseInt(e.target.value)})} 
                                                        style={{fontSize: '12px', padding: '3px 5px', width: '60px'}}
                                                    />
                                                </div>
                                                <div>
                                                    <label style={{fontSize: '12px', marginRight: '5px'}}>高度: </label>
                                                    <input 
                                                        type="number" 
                                                        value={editForm.h} 
                                                        onChange={(e) => setEditForm({...editForm, h: parseInt(e.target.value)})} 
                                                        style={{fontSize: '12px', padding: '3px 5px', width: '60px'}}
                                                    />
                                                </div>
                                            </>
                                        )}
                                        {editingObstacle.type === 'CIRCLE' && (
                                            <div>
                                                <label style={{fontSize: '12px', marginRight: '5px'}}>半径: </label>
                                                <input 
                                                    type="number" 
                                                    value={editForm.r} 
                                                    onChange={(e) => setEditForm({...editForm, r: parseInt(e.target.value)})} 
                                                    style={{fontSize: '12px', padding: '3px 5px', width: '60px'}}
                                                />
                                            </div>
                                        )}
                                        {editingObstacle.type === 'RECT' && (
                                            <div>
                                                <label style={{fontSize: '12px', marginRight: '5px'}}>旋转角度: </label>
                                                <input 
                                                    type="number" 
                                                    step="0.1" 
                                                    value={editForm.angle || 0} 
                                                    onChange={(e) => setEditForm({...editForm, angle: parseFloat(e.target.value)})} 
                                                    style={{fontSize: '12px', padding: '3px 5px', width: '80px'}}
                                                />
                                            </div>
                                        )}
                                        <div>
                                            <label style={{fontSize: '12px', marginRight: '5px'}}>颜色: </label>
                                            <input 
                                                type="color" 
                                                value={editForm.color} 
                                                onChange={(e) => setEditForm({...editForm, color: e.target.value})} 
                                                style={{width: '40px', height: '20px', padding: 0, border: '1px solid #ddd'}}
                                            />
                                        </div>
                                        <div style={{display: 'flex', gap: '8px', marginTop: '10px'}}>
                                            <button onClick={() => {
                                                updateObstacle(editingObstacle.id, editForm);
                                                setEditingObstacle(null);
                                                setEditForm({});
                                            }} style={{
                                                padding: '5px 10px', 
                                                fontSize: '12px',
                                                backgroundColor: '#4ecdc4',
                                                color: 'white',
                                                border: 'none',
                                                borderRadius: '3px',
                                                cursor: 'pointer',
                                                transition: 'background-color 0.2s'
                                            }} onMouseEnter={(e) => e.currentTarget.style.backgroundColor = '#45b7d1'}
                                              onMouseLeave={(e) => e.currentTarget.style.backgroundColor = '#4ecdc4'}>
                                                保存修改
                                            </button>
                                            <button onClick={() => {
                                                setEditingObstacle(null);
                                                setEditForm({});
                                            }} style={{
                                                padding: '5px 10px', 
                                                fontSize: '12px',
                                                backgroundColor: '#95a5a6',
                                                color: 'white',
                                                border: 'none',
                                                borderRadius: '3px',
                                                cursor: 'pointer',
                                                transition: 'background-color 0.2s'
                                            }} onMouseEnter={(e) => e.currentTarget.style.backgroundColor = '#7f8c8d'}
                                              onMouseLeave={(e) => e.currentTarget.style.backgroundColor = '#95a5a6'}>
                                                取消
                                            </button>
                                        </div>
                                    </div>
                                </div>
                            ) : (
                                obstacles.length > 0 ? (
                                    <div style={{
                                        display: 'grid',
                                        gridTemplateColumns: '1fr 1fr',
                                        gap: '10px'
                                    }}>
                                        {obstacles.map(obs => (
                                            <div 
                                                key={obs.id} 
                                                style={{
                                                    border: `1px solid ${obs.id === selectedObstacleId ? '#4ecdc4' : 'rgb(221, 221, 221)'}`,
                                                    borderRadius: '4px',
                                                    padding: '5px',
                                                    background: obs.id === selectedObstacleId ? 'rgba(78, 205, 196, 0.1)' : 'rgb(255, 255, 255)',
                                                    cursor: 'pointer',
                                                    transition: 'all 0.2s ease',
                                                    minHeight: '70px',
                                                    display: 'flex',
                                                    flexDirection: 'column'
                                                }}
                                                onMouseEnter={(e) => {
                                                    e.currentTarget.style.boxShadow = '0 2px 4px rgba(0,0,0,0.1)';
                                                    e.currentTarget.style.transform = 'translateY(-1px)';
                                                }}
                                                onMouseLeave={(e) => {
                                                    e.currentTarget.style.boxShadow = 'none';
                                                    e.currentTarget.style.transform = 'translateY(0)';
                                                }}
                                                onClick={() => setSelectedObstacleId(obs.id)}
                                            >
                                                {/* 第一行：颜色和形状 */}
                                                <div style={{display: 'flex', alignItems: 'center', gap: '4px', marginBottom: '1px'}}>
                                                    <div style={{width: '12px', height: '12px', backgroundColor: obs.color, borderRadius: '2px', border: '1px solid #ddd'}} />
                                                    <span style={{fontSize: '10px', color: '#666', fontWeight: '500'}}>
                                                        {obs.type === 'RECT' ? '矩形' : '圆形'}
                                                    </span>
                                                </div>
                                                
                                                {/* 第二行：ID */}
                                                <div style={{fontSize: '9px', color: '#999', marginBottom: '1px', whiteSpace: 'normal', wordBreak: 'break-all', lineHeight: '1.1'}}>
                                                    ID: {obs.id}
                                                </div>
                                                
                                                {/* 第三行：位置 */}
                                                <div style={{fontSize: '9px', color: '#666', marginBottom: '1px', lineHeight: '1.1'}}>
                                                    位置: ({Math.floor(obs.x)}, {Math.floor(obs.y)})
                                                </div>
                                                
                                                {/* 第四行：尺寸 */}
                                                <div style={{fontSize: '9px', color: '#666', marginBottom: '1px', lineHeight: '1.1'}}>
                                                    {obs.type === 'RECT' ? `尺寸: ${obs.w}x${obs.h}` : `半径: ${obs.r}`}
                                                </div>
                                                
                                                {/* 底部：编辑/删除按钮 */}
                                                <div style={{display: 'flex', justifyContent: 'flex-end', gap: '4px', marginTop: '2px'}}>
                                                    <button 
                                                        onClick={(e) => {
                                                            e.stopPropagation();
                                                            setEditingObstacle(obs);
                                                            setEditForm({...obs});
                                                        }} 
                                                        style={{
                                                            padding: '2px 5px', 
                                                            fontSize: '8px',
                                                            backgroundColor: '#f39c12',
                                                            color: 'white',
                                                            border: 'none',
                                                            borderRadius: '3px',
                                                            cursor: 'pointer',
                                                            transition: 'background-color 0.2s'
                                                        }}
                                                        onMouseEnter={(e) => e.currentTarget.style.backgroundColor = '#e67e22'}
                                                        onMouseLeave={(e) => e.currentTarget.style.backgroundColor = '#f39c12'}
                                                    >
                                                        编辑
                                                    </button>
                                                    <button 
                                                        onClick={(e) => {
                                                            e.stopPropagation();
                                                            removeObstacle(obs.id);
                                                            if (selectedObstacleId === obs.id) {
                                                                setSelectedObstacleId(null);
                                                            }
                                                        }} 
                                                        style={{
                                                            padding: '2px 5px', 
                                                            fontSize: '8px',
                                                            backgroundColor: '#e74c3c',
                                                            color: 'white',
                                                            border: 'none',
                                                            borderRadius: '3px',
                                                            cursor: 'pointer',
                                                            transition: 'background-color 0.2s'
                                                        }}
                                                        onMouseEnter={(e) => e.currentTarget.style.backgroundColor = '#c0392b'}
                                                        onMouseLeave={(e) => e.currentTarget.style.backgroundColor = '#e74c3c'}
                                                    >
                                                        删除
                                                    </button>
                                                </div>
                                            </div>
                                        ))}
                                    </div>
                                ) : (
                                    <div style={{textAlign: 'center', color: '#999', padding: '15px', fontSize: '13px'}}>
                                        暂无障碍物数据
                                    </div>
                                )
                            )}
                        </div>
                    </div>
                    {/* 障碍物创建栏 */}
                    <div style={{
                        marginTop: '20px',
                        padding: '15px',
                        border: '2px solid #333',
                        borderRadius: '8px',
                        background: '#f9f9f9',
                        width: '100%',
                        maxWidth: '280px'
                    }}>
                        <h3 style={{marginTop: 0, fontSize: '16px', textAlign: 'center'}}>创建障碍物</h3>
                        <div style={{
                            display: 'flex',
                            flexDirection: 'column',
                            gap: '10px',
                            alignItems: 'center'
                        }}>
                            <div>
                                <label style={{marginRight: '10px'}}>选择类型:</label>
                                <select 
                                    value={selectedObstacleType} 
                                    onChange={(e) => setSelectedObstacleType(e.target.value as 'RECT' | 'CIRCLE')}
                                    style={{padding: '5px', marginRight: '10px'}}
                                >
                                    <option value="RECT">矩形</option>
                                    <option value="CIRCLE">圆形</option>
                                </select>
                            </div>
                            <div style={{display: 'flex', gap: '10px'}}>
                                <button 
                                    onClick={() => setIsCreatingObstacle(!isCreatingObstacle)}
                                    style={{
                                        padding: '6px 12px',
                                        fontSize: '14px',
                                        backgroundColor: isCreatingObstacle ? '#ff6b6b' : '#4ecdc4',
                                        color: 'white',
                                        border: 'none',
                                        borderRadius: '4px',
                                        cursor: 'pointer'
                                    }}
                                >
                                    {isCreatingObstacle ? '取消' : '开始创建'}
                                </button>
                                <button 
                                    onClick={createObstacleInFrontOfCamera}
                                    style={{
                                        padding: '6px 12px',
                                        fontSize: '14px',
                                        backgroundColor: '#45b7d1',
                                        color: 'white',
                                        border: 'none',
                                        borderRadius: '4px',
                                        cursor: 'pointer'
                                    }}
                                >
                                    在摄像头下创建
                                </button>
                            </div>
                            <div style={{fontSize: '12px', color: '#555', textAlign: 'center'}}>
                                状态: {isCreatingObstacle ? '就绪 - 点击画布创建' : '未激活'}
                            </div>
                        </div>
                    </div>
                </div>
                
                
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

                            <div style={{display: 'flex', gap: '10px'}}>
                                <button onClick={() => sendCommand('ArrowUp')}>指令: 前进</button>
                                <button onClick={() => sendCommand('ArrowLeft')}>指令: 左转</button>
                                <button onClick={() => sendCommand('ArrowRight')}>指令: 右转</button>
                                <button onClick={() => sendCommand('ArrowDown')}>指令: 后退</button>
                                <button onClick={resetCar}>重置 (Reset)</button>
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