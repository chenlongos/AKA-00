import { create } from 'zustand';
import type { Obstacle } from '../model/obstacle';
import { INITIAL_OBSTACLES } from '../model/obstacle';

// 重新导出 Obstacle 类型以保持向后兼容
export type { Obstacle };


interface ObstacleStore {
    obstacles: Obstacle[];
    selectedObstacleId: string | null;
    addObstacle: (obstacle: Omit<Obstacle, 'id'>) => void;
    removeObstacle: (id: string) => void;
    updateObstacle: (id: string, updates: Partial<Obstacle>) => void;
    setObstacles: (obstacles: Obstacle[]) => void;
    clearObstacles: () => void;
    selectObstacle: (id: string | null) => void;
    getObstacleById: (id: string) => Obstacle | undefined;
}

export const useObstacleStore = create<ObstacleStore>((set, get) => ({
    obstacles: INITIAL_OBSTACLES,
    selectedObstacleId: null,
    
    addObstacle: (obstacle) => set((state) => ({
        obstacles: [...state.obstacles, {
            ...obstacle,
            id: Date.now().toString() + Math.random().toString(36).substr(2, 9)
        }]
    })),
    
    removeObstacle: (id) => set((state) => ({
        obstacles: state.obstacles.filter(obs => obs.id !== id),
        selectedObstacleId: state.selectedObstacleId === id ? null : state.selectedObstacleId
    })),
    
    updateObstacle: (id, updates) => set((state) => ({
        obstacles: state.obstacles.map(obs => 
            obs.id === id ? { ...obs, ...updates } : obs
        )
    })),
    
    setObstacles: (obstacles) => set({ obstacles }),
    
    clearObstacles: () => set({ obstacles: [], selectedObstacleId: null }),
    
    selectObstacle: (id) => set({ selectedObstacleId: id }),
    
    getObstacleById: (id) => get().obstacles.find(obs => obs.id === id),
}));
