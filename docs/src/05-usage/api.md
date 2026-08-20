# API 文档

## 控制接口

```
GET /api/control?action=<action>&speed=<speed>&time=<time>&distance=<distance>&angle=<angle>
```

### 参数

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|
| action | string | 是 | up / down / left / right / stop / grab / release |
| speed | int | 否 | 电机百分比（1~100），默认 50 |
| time | int | 否 | 持续时间（毫秒），无 distance/angle 时生效 |
| distance | float | 否 | **移动距离（厘米 cm）**，up/down 有效 |
| angle | float | 否 | **转动角度（度 °）**，left/right 有效 |

> **优先级**：`distance`/`angle` > `time`。传了 distance 或 angle 就忽略 time。
>
> `speed` 是直接发给 ESP32 PID 控制器的目标百分比（`setMotorSpeed(±100)` → `target_rpm = speed × 150 / 100`）。`100%` 对应约 `0.49 m/s`，由 `PWM_RPM_MAX=150 RPM × 轮径62mm × π / 60` 推出。

### 距离运动示例

```bash
# 前进 30 厘米，速度 50%
curl "http://<ip>/api/control?action=up&distance=30&speed=50"

# 后退 15 厘米，速度 30%
curl "http://<ip>/api/control?action=down&distance=15&speed=30"

# 左转 90 度，速度 40%
curl "http://<ip>/api/control?action=left&angle=90&speed=40"

# 右转 45 度（用默认 speed=50）
curl "http://<ip>/api/control?action=right&angle=45"
```

### 时间运动示例

```bash
# 前进 2 秒，速度 50%
curl "http://<ip>/api/control?action=up&speed=50&time=2000"

# 停止
curl "http://<ip>/api/control?action=stop"
```

### 抓取

```bash
curl "http://<ip>/api/control?action=grab"
curl "http://<ip>/api/control?action=release"
```

### speed 物理含义对照

`speed` 是 ESP32 PID 控制器的目标百分比。所有路径（摇杆 / 方向键 / REST+time / REST+distance）共用同一套物理含义：

| speed | 目标 RPM | 约合线速度 |
|-------|---------|-----------|
| 30 | 45 | 0.15 m/s |
| 50 | 75 | 0.24 m/s |
| 100 | 150 | 0.49 m/s |

---

## 电机直接控制

```
GET /api/motor/direct?left=<left>&right=<right>&duration=<duration>
```

| 参数 | 类型 | 说明 |
|------|------|------|
| left | int | 左轮 -100~100 |
| right | int | 右轮 -100~100 |
| duration | float | 持续时间（秒），0 为持续 |

```bash
# 全速前进
curl "http://<ip>/api/motor/direct?left=100&right=100"

# 原地右转
curl "http://<ip>/api/motor/direct?left=50&right=-50"

# 前进 1.5 秒
curl "http://<ip>/api/motor/direct?left=80&right=80&duration=1.5"
```

---

## 电机状态

```
GET /api/motor/status
```

```json
{
  "left_speed": 0.0,
  "right_speed": 0.0,
  "left_target": 50,
  "right_target": 50,
  "gripper_status": "stopped"
}
```

---

## 速度配置

```
GET /api/config/speed
POST /api/config/speed
```

```json
{"forward_speed": 50, "turn_speed": 50}
```

---

## WiFi

### 扫描网络

```
GET /scan
```

### 连接

```
POST /connect
Content-Type: application/json

{"ssid": "WiFi名", "password": "密码"}
```

无密码时 `password` 为空字符串。

### 状态

```
GET /status
GET /api/ip
```

---

## OTA 固件升级

### 当前版本

```
GET /api/ota/version
```

### 检查更新

```
GET /api/ota/check
```

### 在线升级

```
POST /api/ota/upgrade
```

### OTA 状态

```
GET /api/ota/status
```

---

## 系统信息

```
GET /api/system/info
```

```json
{"ip": "192.168.4.1", "mac": "b8:27:eb:xx:xx:xx"}
```

```
GET /api/system/heartbeat
```

返回 CPU、内存、磁盘、运行时间等信息。