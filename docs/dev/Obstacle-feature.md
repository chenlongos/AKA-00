# 障碍物功能文档

## 1. 功能概述

障碍物功能是小车模拟器中的重要组成部分，允许用户在模拟环境中创建、编辑和管理不同类型的障碍物，包括矩形和圆形（球体）障碍物。这些障碍物会与小车进行碰撞检测，影响小车的行驶路径。

## 2. 障碍物类型

### 2.1 支持的类型

| 类型   | 描述               | 关键属性          |
| ------ | ------------------ | ----------------- |
| RECT   | 矩形障碍物         | x, y, w, h, angle |
| CIRCLE | 圆形（球体）障碍物 | x, y, r, angle    |

### 2.2 类型定义

在 `obstacleStore.ts` 中定义了障碍物类型：

```typescript
export type ObstacleType = 'RECT' | 'CIRCLE';

export interface Obstacle {
    id: string;          // 唯一标识符
    x: number;           // 左上角 X 坐标 (RECT) 或圆心 X 坐标 (CIRCLE)
    y: number;           // 左上角 Y 坐标 (RECT) 或圆心 Y 坐标 (CIRCLE)
    w?: number;          // 宽度 (RECT类型使用)
    h?: number;          // 高度 (RECT类型使用)
    r?: number;          // 半径 (CIRCLE类型使用)
    color: string;       // 颜色，十六进制格式
    type: ObstacleType;  // 障碍物类型
    angle?: number;      // 旋转角度 (弧度)
}
```

## 3. 球体障碍物实现

### 3.1 碰撞检测

球体障碍物的碰撞检测使用距离公式实现：

```typescript
else if (obs.type === 'CIRCLE') {
    const dx = x - obs.x;
    const dy = y - obs.y;
    const distance = Math.sqrt(dx * dx + dy * dy);
    return distance < (obs.r || 0);
}
```

### 3.2 绘制实现

球体障碍物使用 Canvas 的 `arc` 方法绘制，封装在 `ObstacleRenderer.tsx` 中：

```typescript
// ObstacleRenderer.tsx
export const renderTopDownObstacles = (ctx, obstacles, selectedObstacleId) => {
    obstacles.forEach(obs => {
        ctx.fillStyle = obs.color;
        if (obs.type === 'RECT') {
            // 矩形绘制逻辑...
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
```

### 3.3 创建实现

创建球体障碍物时的默认参数设置：

```typescript
const newObstacle = {
    type: selectedObstacleType,
    x,
    y,
    w: selectedObstacleType === 'RECT' ? 50 : undefined,
    h: selectedObstacleType === 'RECT' ? 30 : undefined,
    r: selectedObstacleType === 'CIRCLE' ? 20 : undefined,
    color: selectedObstacleType === 'RECT' ? '#8B4513' : '#2E8B57',
    angle: 0
};
```

球体障碍物默认：

- 半径：20
- 颜色：绿色 (#2E8B57)

## 4. 状态管理

使用 Zustand 状态管理库管理障碍物数据：

```typescript
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
```

## 5. 组件架构

### 5.1 模块化架构

障碍物功能已拆分为以下模块：

```
src/
├── model/
│   └── obstacle.ts              # 障碍物类型定义和工具函数
├── store/
│   └── obstacleStore.ts         # Zustand 状态管理
└── components/
    └── obstacle/
        ├── ObstacleRenderer.tsx  # 渲染逻辑（俯视图 + 第一人称）
        └── ObstacleManager.tsx   # UI 管理组件
```

### 5.2 ObstacleRenderer 组件

负责障碍物的渲染逻辑，包括：

| 函数                         | 功能                                   |
| ---------------------------- | -------------------------------------- |
| `renderTopDownObstacles`   | 俯视图障碍物渲染（含选中高亮）         |
| `obstaclesToWalls`         | 将障碍物转换为墙段列表（用于射线投射） |
| `castRay`                  | 射线投射函数                           |
| `computeSprites`           | 计算圆形障碍物精灵数据                 |
| `renderFirstPersonWalls`   | 第一人称视角墙体渲染（含深度缓冲）     |
| `renderFirstPersonSprites` | 第一人称视角精灵渲染（含遮挡处理）     |

**使用示例：**

```typescript
import {
    renderTopDownObstacles,
    obstaclesToWalls,
    computeSprites,
    renderFirstPersonWalls,
    renderFirstPersonSprites
} from "../components/obstacle/ObstacleRenderer";

// 俯视图渲染
renderTopDownObstacles(ctx, obstacles, selectedObstacleId);

// 第一人称渲染
const walls = obstaclesToWalls(obstacles);
const depthBuffer = renderFirstPersonWalls(ctx, walls, carX, carY, carAngle, w, h);
const sprites = computeSprites(obstacles, carX, carY, carAngle, fov, w, h);
renderFirstPersonSprites(ctx, sprites, depthBuffer, rayWidth, rayCount);
```

### 5.3 ObstacleManager 组件

负责障碍物的 UI 管理，包括：

| 子组件               | 功能                             |
| -------------------- | -------------------------------- |
| `ObstacleManager`  | 主组件，管理障碍物列表和创建面板 |
| `ObstacleEditForm` | 编辑表单组件                     |
| `ObstacleItem`     | 障碍物列表项组件                 |
| `ObstacleCreator`  | 创建面板组件                     |

**使用示例：**

```typescript
import {ObstacleManager} from "../components/obstacle/ObstacleManager";

<ObstacleManager onCreateInFront={handleCreateObstacleInFront} />
```

**Props：**

| Prop                | 类型           | 说明                         |
| ------------------- | -------------- | ---------------------------- |
| `onCreateInFront` | `() => void` | 在摄像头前方创建障碍物的回调 |

## 6. 初始障碍物数据

系统默认包含以下障碍物：

```typescript
export const INITIAL_OBSTACLES: Obstacle[] = [
    {id: '1', x: 200, y: 150, w: 100, h: 100, color: '#8e44ad', type: 'RECT'},  // 紫色墙
    {id: '2', x: 400, y: 400, w: 50, h: 150, color: '#e67e22', type: 'RECT'},   // 橙色墙
    {id: '3', x: 100, y: 400, w: 150, h: 50, color: '#16a085', type: 'RECT'},  // 绿色墙
    {id: '4', x: 450, y: 100, w: 50, h: 50, color: '#c0392b', type: 'RECT'},    // 红色柱子
    {id: '5', x: 600, y: 200, r: 40, color: '#3498db', type: 'CIRCLE'},        // 蓝色圆形障碍物
];
```

## 7. UI 交互

### 7.1 ObstacleManager 组件交互

通过 `ObstacleManager` 组件，用户可以：

1. **创建障碍物**

   - 选择障碍物类型（矩形或圆形）
   - 点击「开始创建」按钮进入创建模式
   - 在画布上点击鼠标左键创建障碍物
   - 点击「取消」按钮退出创建模式
   - 点击「在摄像头下创建」按钮在小车当前视角前方创建障碍物
2. **编辑障碍物**

   - 在障碍物列表中找到目标障碍物
   - 点击「编辑」按钮
   - 在弹出的编辑表单中修改属性
   - 点击「保存」按钮应用更改
3. **删除障碍物**

   - 点击「删除」按钮移除障碍物
   - 或选择障碍物后按 Delete 键

### 7.2 画布交互

在画布上，用户可以：

1. 点击选择障碍物
2. 拖拽移动选中的障碍物
3. 使用 Q/E 键旋转选中的障碍物
4. 按 Delete 键删除选中的障碍物
