// 目标物类型枚举
export type TargetType = 'RECT' | 'CIRCLE';

// 目标物接口
export interface Target {
    id: string;
    x: number;
    y: number;
    w?: number;
    h?: number;
    r?: number;
    color: string;
    type: TargetType;
    angle?: number;
}

export const createTarget = (data: Omit<Target, 'id'>): Target => {
    return {
        ...data,
        id: `target_${Date.now()}_${Math.floor(Math.random() * 1000)}`
    };
};

export const isPointInTarget = (x: number, y: number, target: Target): boolean => {
    if (target.type === 'RECT') {
        const width = target.w || 0;
        const height = target.h || 0;

        if (!target.angle) {
            return x > target.x && x < target.x + width &&
                   y > target.y && y < target.y + height;
        }

        const centerX = target.x + width / 2;
        const centerY = target.y + height / 2;

        const dx = x - centerX;
        const dy = y - centerY;
        const rotatedX = dx * Math.cos(-target.angle) - dy * Math.sin(-target.angle);
        const rotatedY = dx * Math.sin(-target.angle) + dy * Math.cos(-target.angle);

        return Math.abs(rotatedX) < width / 2 && Math.abs(rotatedY) < height / 2;
    } else if (target.type === 'CIRCLE') {
        const dx = x - target.x;
        const dy = y - target.y;
        const distance = Math.sqrt(dx * dx + dy * dy);
        return distance < (target.r || 0);
    }
    return false;
};

export const getTargetAtPosition = (x: number, y: number, targets: Target[]): Target | undefined => {
    return targets.find(t => isPointInTarget(x, y, t));
};

export const checkCollision = (x: number, y: number, mapWidth: number, mapHeight: number, targets: Target[]): boolean => {
    if (x < 0 || x > mapWidth || y < 0 || y > mapHeight) return true;
    return targets.some(t => isPointInTarget(x, y, t));
};

export const INITIAL_TARGETS: Target[] = [
    {id: '1', x: 400, y: 170, r: 10, color: '#C7EA46', type: 'CIRCLE'},
];
