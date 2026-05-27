# WebSocket 控制接口

AKA-00 提供 WebSocket 通道用于低延迟实时控制底盘运动，替代 HTTP REST 轮询。

## 连接

```
ws://<机器人IP>/ws/control
```

允许任意来源跨域访问。连接时自动禁用 Nagle 算法，延迟 < 1ms。

## 协议格式

双方通信均为二进制帧。

### 客户端 → 服务端（控制指令）

| Offset | 字节数 | 描述 |
|--------|--------|------|
| 0 | 1 | 帧头 `0xAA` |
| 1 | 1 | X 轴（转向），有符号 int8，-100 ~ 100 |
| 2 | 1 | Y 轴（油门），有符号 int8，-100 ~ 100 |

电机映射：
```
左轮 = Y + X
右轮 = Y - X
```
结果限幅到 `[-100, 100]`。

示例：

| 操作 | 字节 | 说明 |
|------|------|------|
| 前进 50 | `AA 00 32` | X=0, Y=50 |
| 后退 30 | `AA 00 E2` | X=0, Y=-30（补码） |
| 左转 25 | `AA E7 00` | X=-25, Y=0 |
| 右转 25 | `AA 19 00` | X=25, Y=0 |
| 停止 | `AA 00 00` | X=0, Y=0 |

### 服务端 → 客户端（状态上报）

| Offset | 字节数 | 描述 |
|--------|--------|------|
| 0 | 1 | 帧头 `0xBB` |
| 1-2 | 2 | 左轮速度，有符号 int16 LE，单位 m/s × 1000 |
| 3-4 | 2 | 右轮速度，有符号 int16 LE，单位 m/s × 1000 |

推送策略：速度变化时立即推送，无变化时每 2 秒发一次心跳。

示例：

| 速度 | 字节 | 说明 |
|------|------|------|
| 左右均 0.05 m/s | `BB 32 00 32 00` | left=50, right=50 |
| 左 0.03 右 0.01 | `BB 1E 00 0A 00` | left=30, right=10 |
| 停止 | `BB 00 00 00 00` | left=0, right=0 |

## 生命周期

- **连接打开**：服务端注册客户端，启动 200ms 定时器推送状态
- **连接关闭**：服务端自动执行 `run_motor(0, 0)` 停止电机
- **异常断开**：TCP 断开时 Tornado 触发 `on_close`，同样停止电机

## 前端示例

```typescript
const ws = new WebSocket("ws://192.168.4.1/ws/control");
ws.binaryType = "arraybuffer";

// 发送：前进 50
const x = 0, y = 50;
ws.send(new Uint8Array([0xAA, x & 0xFF, y & 0xFF]));

// 接收状态
ws.onmessage = (event) => {
    const buf = new DataView(event.data);
    if (buf.getUint8(0) === 0xBB) {
        const leftSpeed = buf.getInt16(1, true) / 1000;
        const rightSpeed = buf.getInt16(3, true) / 1000;
        console.log(`左: ${leftSpeed} m/s, 右: ${rightSpeed} m/s`);
    }
};
```

## 注意事项

- 连接断开后电机自动停止，无需额外发送停止指令
- 每个 WebSocket 连接独立控制电机，多个连接同时发送会导致竞争
- 控制值限幅 ±100，超出范围会被裁剪
