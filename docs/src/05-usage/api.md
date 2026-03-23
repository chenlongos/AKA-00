# API 文档

## 获取 IP 地址

```
GET /api/ip
```

响应：
```json
{
  "ip": "192.168.1.100"
}
```

## 控制接口

```
GET /api/control?action=<action>&speed=<speed>&time=<time>
```

### 参数

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| action | string | 是 | 动作类型 |
| speed | int | 否 | 速度 0-50 |
| time | int | 否 | 持续时间（毫秒） |

### action 可选值

| 值 | 说明 |
|----|------|
| up | 前进 |
| down | 后退 |
| left | 左转 |
| right | 右转 |
| stop | 停止 |
| grab | 抓取 |
| release | 释放 |

### 示例

```bash
# 前进
curl "http://192.168.1.100/api/control?action=up&speed=30&time=1000"

# 左转
curl "http://192.168.1.100/api/control?action=left&speed=20&time=500"

# 抓取
curl "http://192.168.1.100/api/control?action=grab"

# 释放
curl "http://192.168.1.100/api/control?action=release"
```

## 运行模式

在 `tennis_hunter.py` 中配置：

```python
HARDWARE_MODE = 'rk3588'  # 或 'cpu'
```

| 模式 | 模型 | 说明 |
|------|------|------|
| cpu | best.onnx | 通用 PC 推理 |
| rk3588 | best.rknn | RK3588 NPU 推理 |
