// 障碍物类型枚举
export type ObstacleType = 'RECT' | 'CIRCLE';

// 障碍物接口
export interface Obstacle {
    id: string;          // 唯一标识符，用于区分不同的障碍物
    x: number;           // 障碍物左上角 X 坐标 (RECT) 或圆心 X 坐标 (CIRCLE)
    y: number;           // 障碍物左上角 Y 坐标 (RECT) 或圆心 Y 坐标 (CIRCLE)
    w?: number;          // 障碍物宽度 (RECT类型使用)
    h?: number;          // 障碍物高度 (RECT类型使用)
    r?: number;          // 障碍物半径 (CIRCLE类型使用)
    color: string;       // 障碍物颜色，十六进制格式如 '#8e44ad'
    type: ObstacleType;  // 障碍物类型，用于区分不同形状
    angle?: number;      // 障碍物旋转角度 (弧度)
}

// 创建障碍物的工具函数
export const createObstacle = (data: Omit<Obstacle, 'id'>): Obstacle => {
    return {
        ...data,
        id: `obs_${Date.now()}_${Math.floor(Math.random() * 1000)}`
    };
};

// 检查点是否在障碍物内
export const isPointInObstacle = (x: number, y: number, obstacle: Obstacle): boolean => {
    if (obstacle.type === 'RECT') {
        const width = obstacle.w || 0;
        const height = obstacle.h || 0;
        
        // 如果障碍物没有旋转，使用简单的轴对齐碰撞检测
        if (!obstacle.angle) {
            return x > obstacle.x && x < obstacle.x + width &&
                   y > obstacle.y && y < obstacle.y + height;
        }
        
        // 如果障碍物有旋转，使用旋转碰撞检测
        const centerX = obstacle.x + width / 2;
        const centerY = obstacle.y + height / 2;
        
        // 将点相对于矩形中心点进行旋转
        const dx = x - centerX;
        const dy = y - centerY;
        const rotatedX = dx * Math.cos(-obstacle.angle) - dy * Math.sin(-obstacle.angle);
        const rotatedY = dx * Math.sin(-obstacle.angle) + dy * Math.cos(-obstacle.angle);
        
        // 检查旋转后的点是否在轴对齐的矩形内
        return Math.abs(rotatedX) < width / 2 && Math.abs(rotatedY) < height / 2;
    } else if (obstacle.type === 'CIRCLE') {
        // 圆形碰撞检测
        const dx = x - obstacle.x;
        const dy = y - obstacle.y;
        const distance = Math.sqrt(dx * dx + dy * dy);
        return distance < (obstacle.r || 0);
    }
    return false;
};


// 初始障碍物数据
export const INITIAL_OBSTACLES: Obstacle[] = [
    {id: '1', x: 200, y: 150, w: 100, h: 100, color: '#8e44ad', type: 'RECT'},  // 紫色墙
    {id: '2', x: 400, y: 400, w: 50, h: 150, color: '#e67e22', type: 'RECT'},   // 橙色墙
    {id: '3', x: 100, y: 400, w: 150, h: 50, color: '#16a085', type: 'RECT'},  // 绿色墙
    {id: '4', x: 450, y: 100, w: 50, h: 50, color: '#c0392b', type: 'RECT'},    // 红色柱子
    {id: '5', x: 600, y: 200, r: 40, color: '#3498db', type: 'CIRCLE'},        // 蓝色圆形障碍物
];
