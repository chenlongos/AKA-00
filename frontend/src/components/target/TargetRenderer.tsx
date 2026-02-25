import type {Target} from "../../model/target";

export const MAP_W = 800;
export const MAP_H = 600;

export interface WallSegment {
    x1: number;
    y1: number;
    x2: number;
    y2: number;
    color: string;
}

export interface RayHitResult {
    distance: number;
    color: string | null;
}

export interface SpriteData {
    screenX: number;
    screenY: number;
    size: number;
    dist: number;
    realDist: number;
    color: string;
    isPicked: boolean;
    cx: number;
    cy: number;
    radius: number;
    height: number; // 高度（圆柱体为实际高度，球体为0）
    type: 'CIRCLE' | 'CYLINDER'; // 目标物类型
}

export const getRaySegmentIntersection = (
    rx: number,
    ry: number,
    rdx: number,
    rdy: number,
    wall: WallSegment
): number | null => {
    const {x1, y1, x2, y2} = wall;
    const v1x = x1 - rx;
    const v1y = y1 - ry;
    const v2x = x2 - x1;
    const v2y = y2 - y1;
    const v3x = -rdx;
    const v3y = -rdy;

    const cross = v2x * v3y - v2y * v3x;
    if (Math.abs(cross) < 0.0001) return null;

    const t1 = (v2x * v1y - v2y * v1x) / cross;
    const t2 = (v3x * v1y - v3y * v1x) / cross;

    if (t1 > 0 && t2 >= 0 && t2 <= 1) {
        return t1;
    }
    return null;
};

export const targetsToWalls = (targets: Target[]): WallSegment[] => {
    const walls: WallSegment[] = [
        {x1: 0, y1: 0, x2: MAP_W, y2: 0, color: '#333'},
        {x1: MAP_W, y1: 0, x2: MAP_W, y2: MAP_H, color: '#333'},
        {x1: MAP_W, y1: MAP_H, x2: 0, y2: MAP_H, color: '#333'},
        {x1: 0, y1: MAP_H, x2: 0, y2: 0, color: '#333'}
    ];

    targets.forEach(t => {
        const c = t.color;
        if (t.type === 'RECT') {
            const width = t.w || 0;
            const height = t.h || 0;

            let vertices = [
                {x: t.x, y: t.y},
                {x: t.x + width, y: t.y},
                {x: t.x + width, y: t.y + height},
                {x: t.x, y: t.y + height}
            ];

            if (t.angle) {
                const centerX = t.x + width / 2;
                const centerY = t.y + height / 2;

                vertices = vertices.map(vertex => {
                    const dx = vertex.x - centerX;
                    const dy = vertex.y - centerY;
                    const angle = t.angle || 0;
                    const rotatedX = dx * Math.cos(angle) - dy * Math.sin(angle);
                    const rotatedY = dx * Math.sin(angle) + dy * Math.cos(angle);
                    return {
                        x: rotatedX + centerX,
                        y: rotatedY + centerY
                    };
                });
            }

            walls.push({x1: vertices[0].x, y1: vertices[0].y, x2: vertices[1].x, y2: vertices[1].y, color: c});
            walls.push({x1: vertices[1].x, y1: vertices[1].y, x2: vertices[2].x, y2: vertices[2].y, color: c});
            walls.push({x1: vertices[2].x, y1: vertices[2].y, x2: vertices[3].x, y2: vertices[3].y, color: c});
            walls.push({x1: vertices[3].x, y1: vertices[3].y, x2: vertices[0].x, y2: vertices[0].y, color: c});
        }
        // 圆柱体不再生成墙段，将作为精灵渲染
    });

    return walls;
};

export const castRay = (
    sx: number,
    sy: number,
    angle: number,
    walls: WallSegment[]
): RayHitResult | null => {
    const cos = Math.cos(angle);
    const sin = Math.sin(angle);
    let minDist = Infinity;
    let hitColor: string | null = null;

    walls.forEach(wall => {
        const dist = getRaySegmentIntersection(sx, sy, cos, sin, wall);
        if (dist !== null && dist < minDist) {
            minDist = dist;
            hitColor = wall.color;
        }
    });

    return minDist === Infinity ? null : {distance: minDist, color: hitColor};
};

export const renderTopDownTargets = (
    ctx: CanvasRenderingContext2D,
    targets: Target[],
    selectedTargetId: string | null
): void => {
    targets.forEach(t => {
        ctx.fillStyle = t.color;
        if (t.type === 'RECT') {
            const width = t.w || 0;
            const height = t.h || 0;
            const centerX = t.x + width / 2;
            const centerY = t.y + height / 2;

            ctx.save();

            if (t.angle) {
                ctx.translate(centerX, centerY);
                ctx.rotate(t.angle);
                ctx.translate(-centerX, -centerY);
            }

            ctx.fillRect(t.x, t.y, width, height);
            ctx.strokeStyle = '#333';
            ctx.strokeRect(t.x, t.y, width, height);

            if (t.id === selectedTargetId) {
                ctx.strokeStyle = '#ff0000';
                ctx.lineWidth = 2;
                ctx.setLineDash([5, 5]);
                ctx.strokeRect(t.x - 5, t.y - 5, width + 10, height + 10);
                ctx.setLineDash([]);
                ctx.lineWidth = 1;
            }

            ctx.restore();
        } else if (t.type === 'CIRCLE' || t.type === 'CYLINDER') {
            ctx.beginPath();
            ctx.arc(t.x, t.y, t.r || 0, 0, Math.PI * 2);
            ctx.fill();
            
            // 为圆柱体添加特殊标记以区分球体
            if (t.type === 'CYLINDER') {
                ctx.strokeStyle = '#0000FF'; // 蓝色边框区分
                ctx.lineWidth = 2;
                // 添加十字标记
                ctx.beginPath();
                ctx.moveTo(t.x - (t.r || 0), t.y);
                ctx.lineTo(t.x + (t.r || 0), t.y);
                ctx.moveTo(t.x, t.y - (t.r || 0));
                ctx.lineTo(t.x, t.y + (t.r || 0));
                ctx.stroke();
                ctx.lineWidth = 1;
            } else {
                ctx.strokeStyle = '#333';
            }
            
            ctx.stroke();

            if (t.id === selectedTargetId) {
                ctx.strokeStyle = '#ff0000';
                ctx.lineWidth = 2;
                ctx.setLineDash([5, 5]);
                ctx.beginPath();
                ctx.arc(t.x, t.y, (t.r || 0) + 5, 0, Math.PI * 2);
                ctx.stroke();
                ctx.setLineDash([]);
                ctx.lineWidth = 1;
            }
        }
    });
};

export const computeSprites = (
  targets: Target[],
  carX: number,
  carY: number,
  carAngle: number,
  fov: number,
  w: number,
  h: number
): SpriteData[] => {
  const balls = targets.filter(t => t.type === 'CIRCLE' || t.type === 'CYLINDER');
  const halfFov = fov / 2;
  const tanHalfFov = Math.tan(halfFov);
  const eyeHeight = 20;

  return balls.map(ball => {
    const dx = ball.x - carX;
    const dy = ball.y - carY;
    const dist = Math.hypot(dx, dy);
    const radius = ball.r || 15;
    const height = ball.type === 'CYLINDER' ? (ball.h || 40) : 0;

    const cosCar = Math.cos(carAngle);
    const sinCar = Math.sin(carAngle);
    const tx = dx * cosCar + dy * sinCar;
    const ty = -dx * sinCar + dy * cosCar;

    if (tx <= 0.001) return null;

    const screenX = w / 2 + (ty / tx) * (w / 2) / tanHalfFov;

    const spriteSize = (h * 2 * radius) / tx;

    // 圆柱体需要调整屏幕Y位置，因为有一定高度
    const screenY = h / 2 + (eyeHeight - (ball.type === 'CYLINDER' ? height / 2 : radius)) * h / tx;

    return {
      screenX,
      screenY,
      size: spriteSize,
      dist: tx,
      realDist: dist,
      color: ball.color,
      isPicked: false,
      cx: ball.x,
      cy: ball.y,
      radius,
      height: ball.type === 'CYLINDER' ? height : 0,
      type: ball.type
    };
  }).filter((sprite): sprite is SpriteData => sprite !== null)
    .sort((a, b) => b.realDist - a.realDist);
};

const raySphereHit = (ox: number, oy: number, dx: number, dy: number, cx: number, cy: number, r: number): number | null => {
    const ocx = ox - cx, ocy = oy - cy;
    const b = dx * ocx + dy * ocy;
    const c = ocx * ocx + ocy * ocy - r * r;
    const d = b * b - c;
    if (d < 0) return null;
    const t = -b - Math.sqrt(d);
    return t > 0.0001 ? t : (-b + Math.sqrt(d) > 0.0001 ? -b + Math.sqrt(d) : null);
};

export const renderFirstPersonWalls = (
    ctx: CanvasRenderingContext2D,
    walls: WallSegment[],
    carX: number,
    carY: number,
    carAngle: number,
    w: number,
    h: number
): number[] => {
    const fov = Math.PI / 3;
    const rayCount = w / 4;
    const rayWidth = w / rayCount;
    const depthBuffer = new Array(rayCount).fill(Infinity);

    for (let i = 0; i < rayCount; i++) {
        const rayAngle = (carAngle + Math.PI - fov / 2) + (i / rayCount) * fov;
        const hit = castRay(carX, carY, rayAngle, walls);

        if (hit) {
            const correctedDist = hit.distance * Math.cos(rayAngle - carAngle);
            const wallHeight = (h * 40) / correctedDist;
            depthBuffer[i] = Math.abs(correctedDist);

            ctx.fillStyle = hit.color ?? '#000';
            ctx.globalAlpha = Math.max(0.3, 1 - correctedDist / 600);
            ctx.fillRect(i * rayWidth, (h - wallHeight) / 2, rayWidth + 1, wallHeight);
            ctx.globalAlpha = 1.0;
        }
    }

    return depthBuffer;
};

export const renderFirstPersonSprites = (
    ctx: CanvasRenderingContext2D,
    sprites: SpriteData[],
    depthBuffer: number[],
    rayWidth: number,
    rayCount: number,
    carX: number,
    carY: number,
    carAngle: number,
    fov: number,
    h: number
): void => {
    const halfFov = fov / 2;
    sprites.forEach(s => {
        const left = Math.max(0, Math.floor((s.screenX - s.size / 2) / rayWidth));
        const right = Math.min(rayCount - 1, Math.ceil((s.screenX + s.size / 2) / rayWidth));

        for (let i = left; i <= right; i++) {
            const rayAngle = carAngle - halfFov + (i / rayCount) * fov;
            const dx = Math.cos(rayAngle), dy = Math.sin(rayAngle);
            const t = raySphereHit(carX, carY, dx, dy, s.cx, s.cy, s.radius);
            if (!t) continue;

            const tProj = t * Math.cos(rayAngle - carAngle);
            if (tProj >= depthBuffer[i]) continue;

            const relX = (i * rayWidth + rayWidth / 2 - s.screenX) / (s.size / 2);
            if (Math.abs(relX) > 1) continue;

            let colHeight: number;
            if (s.type === 'CYLINDER') {
                // 圆柱体：渲染为矩形（长方形）
                // 使用固定高度，不随水平位置变化
                const cylinderHeight = s.height * h / s.dist; // 将实际高度转换为屏幕高度
                colHeight = cylinderHeight; // 固定高度，形成矩形
            } else {
                // 球体：原有的球形曲面
                colHeight = Math.sqrt(1 - relX * relX) * s.size;
            }
            
            const colTop = s.screenY - colHeight / 2;

            ctx.fillStyle = s.color;
            if (s.isPicked) { ctx.shadowColor = '#0f0'; ctx.shadowBlur = 15; }
            ctx.fillRect(i * rayWidth, colTop, rayWidth + 1, colHeight);
            if (s.isPicked) ctx.shadowBlur = 0;
        }
    });
};
