# 障碍物功能文档

## 1. 功能概述

障碍物功能是小车模拟器中的重要组成部分，允许用户在模拟环境中创建、编辑和管理不同类型的障碍物，包括矩形和圆形（球体）障碍物。这些障碍物会与小车进行碰撞检测，影响小车的行驶路径。

## 2. 障碍物类型

### 2.1 支持的类型

| 类型 | 描述 | 关键属性 |
|------|------|----------|
| RECT | 矩形障碍物 | x, y, w, h, angle |
| CIRCLE | 圆形（球体）障碍物 | x, y, r, angle |

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
    // 圆形碰撞检测
    const dx = x - obs.x;
    const dy = y - obs.y;
    const distance = Math.sqrt(dx * dx + dy * dy);
    return distance < (obs.r || 0);
}
```

### 3.2 绘制实现

球体障碍物使用 Canvas 的 `arc` 方法绘制：

```typescript
else if (obs.type === 'CIRCLE') {
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
```

### 3.3 创建实现

创建球体障碍物时的默认参数设置：

```typescript
const newObstacle = {
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
```

## 5. 初始障碍物数据

系统默认包含以下障碍物，其中包括一个球体障碍物：

```typescript
export const INITIAL_OBSTACLES: Obstacle[] = [
    {id: '1', x: 200, y: 150, w: 100, h: 100, color: '#8e44ad', type: 'RECT'},  // 紫色墙
    {id: '2', x: 400, y: 400, w: 50, h: 150, color: '#e67e22', type: 'RECT'},   // 橙色墙
    {id: '3', x: 100, y: 400, w: 150, h: 50, color: '#16a085', type: 'RECT'},  // 绿色墙
    {id: '4', x: 450, y: 100, w: 50, h: 50, color: '#c0392b', type: 'RECT'},    // 红色柱子
    {id: '5', x: 600, y: 200, r: 40, color: '#3498db', type: 'CIRCLE'},        // 蓝色圆形障碍物
];
```

## 6. UI 交互

### 6.1 障碍物创建栏

在界面左侧的障碍物创建栏中，用户可以：

1. 选择障碍物类型（矩形或圆形）
2. 点击「开始创建」按钮进入创建模式
3. 在画布上点击鼠标左键创建障碍物
4. 点击「取消」按钮退出创建模式
5. 点击「在摄像头下创建」按钮在小车当前摄像头视角前方创建障碍物

### 6.2 障碍物列表

界面左侧显示所有已创建的障碍物，每个障碍物条目包含：

1. 障碍物类型和 ID
2. 编辑按钮：修改障碍物属性
3. 删除按钮：移除障碍物

### 6.3 画布交互

在画布上，用户可以：

1. 点击选择障碍物
2. 拖拽移动选中的障碍物
3. 使用 Q/E 键旋转选中的障碍物
4. 按 Delete 键删除选中的障碍物

## 7. 碰撞检测与物理引擎

### 7.1 碰撞检测

障碍物与小车的碰撞检测逻辑：

1. 对于矩形障碍物：使用旋转矩形碰撞检测算法
2. 对于球体障碍物：使用距离公式检测

### 7.2 碰撞响应

当检测到碰撞时，系统会：

1. 记录碰撞事件
2. 可能会影响小车的行驶状态（具体逻辑取决于后端实现）

## 8. 性能优化

1. **使用 useRef 存储障碍物引用**：解决渲染循环中访问最新状态的闭包问题
2. **批量更新**：使用 Zustand 的状态更新机制，避免频繁重渲染
3. **Canvas 绘制优化**：使用 Canvas API 直接绘制，减少 DOM 操作

## 9. 使用示例

### 9.1 创建球体障碍物

1. 在障碍物创建栏中选择「圆形」类型
2. 点击「开始创建」按钮
3. 在画布上点击鼠标左键，系统会在点击位置创建一个默认大小的球体障碍物

### 9.2 编辑球体障碍物

1. 在障碍物列表中找到目标球体障碍物
2. 点击「编辑」按钮
3. 在弹出的编辑表单中修改半径、颜色等属性
4. 点击「保存」按钮应用更改

### 9.3 删除球体障碍物

1. 在障碍物列表中找到目标球体障碍物
2. 点击「删除」按钮
3. 或选择障碍物后按 Delete 键

## 10. 技术要点

1. **TypeScript 类型安全**：使用 TypeScript 接口定义障碍物类型，确保类型安全
2. **Zustand 状态管理**：轻量级状态管理，简化数据流
3. **Canvas 2D 绘图**：高效绘制障碍物和小车
4. **事件处理**：响应式处理用户交互事件
5. **碰撞检测算法**：实现精确的碰撞检测逻辑

## 11. 未来扩展

1. **支持更多形状**：如三角形、多边形等
2. **障碍物属性预设**：提供常用障碍物模板
3. **障碍物分组**：支持障碍物的批量操作
4. **障碍物复制**：快速复制现有障碍物
5. **导入/导出**：支持障碍物配置的导入导出

## 12. 总结

障碍物功能为小车模拟器提供了丰富的环境构建能力，特别是球体障碍物的实现，为模拟真实世界中的圆形物体（如球体、柱子等）提供了支持。通过直观的 UI 交互和高效的状态管理，用户可以轻松创建和管理各种障碍物，构建复杂的模拟场景。