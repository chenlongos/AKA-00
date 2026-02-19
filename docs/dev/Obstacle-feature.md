# 目标物功能文档

## 1. 功能概述

目标物功能是小车模拟器中的重要组成部分，允许用户在模拟环境中创建、编辑和管理不同类型的物体，包括矩形和圆形（球体）目标物。这些目标物会与小车进行碰撞检测，影响小车的行驶路径。

## 2. 目标物类型

### 2.1 支持的类型

| 类型   | 描述               | 关键属性          |
| ------ | ------------------ | ----------------- |
| RECT   | 矩形目标物         | x, y, w, h, angle |
| CIRCLE | 圆形（球体）目标物 | x, y, r, angle    |

### 2.2 类型定义

在 `targetStore.ts` 中定义了目标物类型：

```typescript
export type TargetType = 'RECT' | 'CIRCLE';

export interface Target {
    id: string;          // 唯一标识符
    x: number;           // 左上角 X 坐标 (RECT) 或圆心 X 坐标 (CIRCLE)
    y: number;           // 左上角 Y 坐标 (RECT) 或圆心 Y 坐标 (CIRCLE)
    w?: number;          // 宽度 (RECT类型使用)
    h?: number;          // 高度 (RECT类型使用)
    r?: number;          // 半径 (CIRCLE类型使用)
    color: string;       // 颜色，十六进制格式
    type: TargetType;  // 目标物类型
    angle?: number;      // 旋转角度 (弧度)
}
```

## 3. 球体目标物实现

### 3.1 碰撞检测

球体目标物的碰撞检测使用距离公式实现：

```typescript
else if (t.type === 'CIRCLE') {
    const dx = x - t.x;
    const dy = y - t.y;
    const distance = Math.sqrt(dx * dx + dy * dy);
    return distance < (t.r || 0);
}
```

### 3.2 绘制实现

球体目标物使用 Canvas 的 `arc` 方法绘制，封装在 `TargetRenderer.tsx` 中：

```typescript
// TargetRenderer.tsx
export const renderTopDownTargets = (ctx, targets, selectedTargetId) => {
    targets.forEach(t => {
        ctx.fillStyle = t.color;
        if (t.type === 'RECT') {
            // 矩形绘制逻辑...
        } else if (t.type === 'CIRCLE') {
            ctx.beginPath();
            ctx.arc(t.x, t.y, t.r || 0, 0, Math.PI * 2);
            ctx.fill();
            ctx.strokeStyle = '#333';
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
```

### 3.3 创建实现

创建球体目标物时的默认参数设置：

```typescript
const newTarget = {
    type: selectedTargetType,
    x,
    y,
    w: selectedTargetType === 'RECT' ? 50 : undefined,
    h: selectedTargetType === 'RECT' ? 30 : undefined,
    r: selectedTargetType === 'CIRCLE' ? 20 : undefined,
    color: selectedTargetType === 'RECT' ? '#8B4513' : '#2E8B57',
    angle: 0
};
```

球体目标物默认：

- 半径：20
- 颜色：绿色 (#2E8B57)

## 4. 状态管理

使用 Zustand 状态管理库管理目标物数据：

```typescript
interface TargetStore {
    targets: Target[];
    selectedTargetId: string | null;
    addTarget: (target: Omit<Target, 'id'>) => void;
    removeTarget: (id: string) => void;
    updateTarget: (id: string, updates: Partial<Target>) => void;
    setTargets: (targets: Target[]) => void;
    clearTargets: () => void;
    selectTarget: (id: string | null) => void;
    getTargetById: (id: string) => Target | undefined;
}

export const useTargetStore = create<TargetStore>((set, get) => ({
    targets: INITIAL_TARGETS,
    selectedTargetId: null,

    addTarget: (target) => set((state) => ({
        targets: [...state.targets, {
            ...target,
            id: Date.now().toString() + Math.random().toString(36).substr(2, 9)
        }]
    })),

    removeTarget: (id) => set((state) => ({
        targets: state.targets.filter(t => t.id !== id),
        selectedTargetId: state.selectedTargetId === id ? null : state.selectedTargetId
    })),

    updateTarget: (id, updates) => set((state) => ({
        targets: state.targets.map(t =>
            t.id === id ? { ...t, ...updates } : t
        )
    })),

    setTargets: (targets) => set({ targets }),

    clearTargets: () => set({ targets: [], selectedTargetId: null }),

    selectTarget: (id) => set({ selectedTargetId: id }),

    getTargetById: (id) => get().targets.find(t => t.id === id),
}));
```

## 5. 组件架构

### 5.1 模块化架构

目标物功能已拆分为以下模块：

```
src/
├── model/
│   └── target.ts              # 目标物类型定义和工具函数
├── store/
│   └── targetStore.ts         # Zustand 状态管理
└── components/
    └── target/
        ├── TargetRenderer.tsx  # 渲染逻辑（俯视图 + 第一人称）
        └── TargetManager.tsx   # UI 管理组件
```

### 5.2 TargetRenderer 组件

负责目标物的渲染逻辑，包括：

| 函数                         | 功能                                   |
| ---------------------------- | -------------------------------------- |
| `renderTopDownTargets`   | 俯视图目标物渲染（含选中高亮）         |
| `targetsToWalls`         | 将目标物转换为墙段列表（用于射线投射） |
| `castRay`                  | 射线投射函数                           |
| `computeSprites`           | 计算圆形目标物精灵数据                 |
| `renderFirstPersonWalls`   | 第一人称视角墙体渲染（含深度缓冲）     |
| `renderFirstPersonSprites` | 第一人称视角精灵渲染（含遮挡处理）     |

**使用示例：**

```typescript
import {
    renderTopDownTargets,
    targetsToWalls,
    computeSprites,
    renderFirstPersonWalls,
    renderFirstPersonSprites
} from "../components/target/TargetRenderer";

// 俯视图渲染
renderTopDownTargets(ctx, targets, selectedTargetId);

// 第一人称渲染
const walls = targetsToWalls(targets);
const depthBuffer = renderFirstPersonWalls(ctx, walls, carX, carY, carAngle, w, h);
const sprites = computeSprites(targets, carX, carY, carAngle, fov, w, h);
renderFirstPersonSprites(ctx, sprites, depthBuffer, rayWidth, rayCount);
```

### 5.3 TargetManager 组件

负责目标物的 UI 管理，包括：

| 子组件               | 功能                             |
| -------------------- | -------------------------------- |
| `TargetManager`  | 主组件，管理目标物列表和创建面板 |
| `TargetEditForm` | 编辑表单组件                     |
| `TargetItem`     | 目标物列表项组件                 |
| `TargetCreator`  | 创建面板组件                     |

**使用示例：**

```typescript
import {TargetManager} from "../components/target/TargetManager";

<TargetManager onCreateInFront={handleCreateTargetInFront} />
```

**Props：**

| Prop                | 类型           | 说明                         |
| ------------------- | -------------- | ---------------------------- |
| `onCreateInFront` | `() => void` | 在摄像头前方创建目标物的回调 |

## 6. 初始目标物数据

系统默认包含以下目标物：

```typescript
export const INITIAL_TARGETS: Target[] = [
    {id: '1', x: 200, y: 150, w: 100, h: 100, color: '#8e44ad', type: 'RECT'},  // 紫色墙
    {id: '2', x: 400, y: 400, w: 50, h: 150, color: '#e67e22', type: 'RECT'},   // 橙色墙
    {id: '3', x: 100, y: 400, w: 150, h: 50, color: '#16a085', type: 'RECT'},  // 绿色墙
    {id: '4', x: 450, y: 100, w: 50, h: 50, color: '#c0392b', type: 'RECT'},    // 红色柱子
    {id: '5', x: 600, y: 200, r: 40, color: '#3498db', type: 'CIRCLE'},        // 蓝色圆形目标物
];
```

## 7. UI 交互

### 7.1 TargetManager 组件交互

通过 `TargetManager` 组件，用户可以：

1. **创建目标物**

   - 选择目标物类型（矩形或圆形）
   - 点击「开始创建」按钮进入创建模式
   - 在画布上点击鼠标左键创建目标物
   - 点击「取消」按钮退出创建模式
   - 点击「在摄像头下创建」按钮在小车当前视角前方创建目标物
2. **编辑目标物**

   - 在目标物列表中找到目标目标物
   - 点击「编辑」按钮
   - 在弹出的编辑表单中修改属性
   - 点击「保存」按钮应用更改
3. **删除目标物**

   - 点击「删除」按钮移除目标物
   - 或选择目标物后按 Delete 键

### 7.2 画布交互

在画布上，用户可以：

1. 点击选择目标物
2. 拖拽移动选中的目标物
3. 使用 Q/E 键旋转选中的目标物
4. 按 Delete 键删除选中的目标物
