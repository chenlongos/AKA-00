# API 文档

## 控制接口

```
GET /api/control?action=<action>&speed=<speed>&time=<time>&distance=<distance>&angle=<angle>
```

### 参数

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| action | string | 是 | up / down / left / right / stop / grab / release |
| speed | float | 否 | 线速度 m/s，范围 0.01~0.5，默认 0.25 |
| time | int | 否 | 持续时间（毫秒），无 distance/angle 时生效 |
| distance | float | 否 | **移动距离（厘米 cm）**，up/down 有效 |
| angle | float | 否 | **转动角度（度 °）**，left/right 有效 |

> **优先级**：`distance`/`angle` > `time`。传了 distance 或 angle 就忽略 time。

### 距离运动示例

```bash
# 前进 30 厘米，速度 0.3 m/s
curl "http://<ip>/api/control?action=up&distance=30&speed=0.3"

# 后退 15 厘米
curl "http://<ip>/api/control?action=down&distance=15&speed=0.25"

# 左转 90 度
curl "http://<ip>/api/control?action=left&angle=90&speed=0.2"

# 右转 45 度
curl "http://<ip>/api/control?action=right&angle=45"
```

### 时间运动示例

```bash
# 前进 2 秒，速度 0.3 m/s
curl "http://<ip>/api/control?action=up&speed=0.3&time=2000"

# 停止
curl "http://<ip>/api/control?action=stop"
```

### 抓取

```bash
curl "http://<ip>/api/control?action=grab"
curl "http://<ip>/api/control?action=release"
```

### 速度换算

speed（m/s）和电机百分比的关系：
| m/s | 电机 % |
|-----|--------|
| 0.10 | 20% |
| 0.25 | 50% |
| 0.40 | 80% |
| 0.50 | 100% |

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
