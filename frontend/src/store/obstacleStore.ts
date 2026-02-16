import { create } from 'zustand';

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

// 初始障碍物数据
export const INITIAL_OBSTACLES: Obstacle[] = [
    {id: '1', x: 200, y: 150, w: 100, h: 100, color: '#8e44ad', type: 'RECT'},  // 紫色墙
    {id: '2', x: 400, y: 400, w: 50, h: 150, color: '#e67e22', type: 'RECT'},   // 橙色墙
    {id: '3', x: 100, y: 400, w: 150, h: 50, color: '#16a085', type: 'RECT'},  // 绿色墙
    {id: '4', x: 450, y: 100, w: 50, h: 50, color: '#c0392b', type: 'RECT'},    // 红色柱子
    {id: '5', x: 600, y: 200, r: 40, color: '#3498db', type: 'CIRCLE'},        // 蓝色圆形障碍物
];

interface ObstacleStore {
    obstacles: Obstacle[];
    addObstacle: (obstacle: Omit<Obstacle, 'id'>) => void;
    removeObstacle: (id: string) => void;
    updateObstacle: (id: string, updates: Partial<Obstacle>) => void;
    setObstacles: (obstacles: Obstacle[]) => void;
    clearObstacles: () => void;
}

export const useObstacleStore = create<ObstacleStore>((set) => ({
    obstacles: INITIAL_OBSTACLES,
    
    addObstacle: (obstacle) => set((state) => ({
        obstacles: [...state.obstacles, {
            ...obstacle,
            id: Date.now().toString() + Math.random().toString(36).substr(2, 9)
        }]
    })),
    
    removeObstacle: (id) => set((state) => ({
        obstacles: state.obstacles.filter(obs => obs.id !== id)
    })),
    
    updateObstacle: (id, updates) => set((state) => ({
        obstacles: state.obstacles.map(obs => 
            obs.id === id ? { ...obs, ...updates } : obs
        )
    })),
    
    setObstacles: (obstacles) => set({ obstacles }),
    
    clearObstacles: () => set({ obstacles: [] }),
}));
