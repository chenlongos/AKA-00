import type {Obstacle} from "../../model/obstacle";

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

export const obstaclesToWalls = (obstacles: Obstacle[]): WallSegment[] => {
    const walls: WallSegment[] = [
        {x1: 0, y1: 0, x2: MAP_W, y2: 0, color: '#333'},
        {x1: MAP_W, y1: 0, x2: MAP_W, y2: MAP_H, color: '#333'},
        {x1: MAP_W, y1: MAP_H, x2: 0, y2: MAP_H, color: '#333'},
        {x1: 0, y1: MAP_H, x2: 0, y2: 0, color: '#333'}
    ];

    obstacles.forEach(obs => {
        const c = obs.color;
        if (obs.type === 'RECT') {
            const width = obs.w || 0;
            const height = obs.h || 0;

            let vertices = [
                {x: obs.x, y: obs.y},
                {x: obs.x + width, y: obs.y},
                {x: obs.x + width, y: obs.y + height},
                {x: obs.x, y: obs.y + height}
            ];

            if (obs.angle) {
                const centerX = obs.x + width / 2;
                const centerY = obs.y + height / 2;

                vertices = vertices.map(vertex => {
                    const dx = vertex.x - centerX;
                    const dy = vertex.y - centerY;
                    const angle = obs.angle || 0;
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

export const renderTopDownObstacles = (
    ctx: CanvasRenderingContext2D,
    obstacles: Obstacle[],
    selectedObstacleId: string | null
): void => {
    obstacles.forEach(obs => {
        ctx.fillStyle = obs.color;
        if (obs.type === 'RECT') {
            const width = obs.w || 0;
            const height = obs.h || 0;
            const centerX = obs.x + width / 2;
            const centerY = obs.y + height / 2;

            ctx.save();

            if (obs.angle) {
                ctx.translate(centerX, centerY);
                ctx.rotate(obs.angle);
                ctx.translate(-centerX, -centerY);
            }

            ctx.fillRect(obs.x, obs.y, width, height);
            ctx.strokeStyle = '#333';
            ctx.strokeRect(obs.x, obs.y, width, height);

            if (obs.id === selectedObstacleId) {
                ctx.strokeStyle = '#ff0000';
                ctx.lineWidth = 2;
                ctx.setLineDash([5, 5]);
                ctx.strokeRect(obs.x - 5, obs.y - 5, width + 10, height + 10);
                ctx.setLineDash([]);
                ctx.lineWidth = 1;
            }

            ctx.restore();
        } else if (obs.type === 'CIRCLE') {
            ctx.beginPath();
            ctx.arc(obs.x, obs.y, obs.r || 0, 0, Math.PI * 2);
            ctx.fill();
            ctx.strokeStyle = '#333';
            ctx.stroke();

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
};

export const computeSprites = (
    obstacles: Obstacle[],
    carX: number,
    carY: number,
    carAngle: number,
    fov: number,
    w: number,
    h: number
): SpriteData[] => {
    const balls = obstacles.filter(obs => obs.type === 'CIRCLE');

    return balls.map(ball => {
        const dx = ball.x - carX;
        const dy = ball.y - carY;
        const dist = Math.sqrt(dx * dx + dy * dy);

        let spriteAngle = Math.atan2(dy, dx) - carAngle;
        while (spriteAngle > Math.PI) spriteAngle -= Math.PI * 2;
        while (spriteAngle < -Math.PI) spriteAngle += Math.PI * 2;

        const correctedDist = Math.abs(dist * Math.cos(spriteAngle));
        const screenX = (w / 2) + (spriteAngle / fov) * w;
        const spriteSize = (h * 40) / correctedDist * ((ball.r || 15) / 15);
        const wallHeight = (h * 40) / correctedDist;
        const wallBottom = h / 2 + wallHeight / 2;
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
    }).sort((a, b) => b.realDist - a.realDist);
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
    rayCount: number
): void => {
    sprites.forEach(sprite => {
        if (sprite.dist < 1 || sprite.realDist > 1000) {
            return;
        }

        const leftCol = Math.max(0, Math.floor((sprite.screenX - sprite.size / 2) / rayWidth));
        const rightCol = Math.min(rayCount - 1, Math.ceil((sprite.screenX + sprite.size / 2) / rayWidth));

        for (let i = leftCol; i <= rightCol; i++) {
            if (sprite.dist >= depthBuffer[i]) {
                continue;
            }

            const colScreenX = i * rayWidth;
            const relX = (colScreenX + rayWidth / 2 - sprite.screenX) / (sprite.size / 2);

            if (Math.abs(relX) > 1) {
                continue;
            }

            const relY = Math.sqrt(1 - relX * relX);
            const colHeight = relY * sprite.size;
            const colTop = sprite.screenY - colHeight / 2;

            ctx.save();
            ctx.fillStyle = sprite.color;
            ctx.globalAlpha = 1.0;

            if (sprite.isPicked) {
                ctx.shadowColor = '#00ff00';
                ctx.shadowBlur = 15;
            }

            ctx.fillRect(colScreenX, colTop, rayWidth + 1, colHeight);
            ctx.restore();
        }
    });
};
