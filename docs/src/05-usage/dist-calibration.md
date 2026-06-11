# 距离标定

小车支持通过 YOLO 视觉检测 + 标定公式估算目标物体的真实距离。

## 标定公式

```
D = m / P + c
```

| 符号 | 含义 | 单位 |
|------|------|------|
| D | 目标物体的真实距离 | cm |
| P | 检测框在画面中的像素尺寸（取宽高中的较大值） | px |
| m | 标定乘数，由相机焦距和物体实际尺寸决定 | — |
| c | 标定偏移，修正系统误差 | — |

## 标定参数

标定参数 `m` 和 `c` 配置在 `app/config.py` 的 `HardwareConfig` 中：

```python
@dataclass(frozen=True)
class HardwareConfig:
    # ...
    # 距离标定: D = m / P + c
    calib_m: float = 2671.82
    calib_c: float = -2.82
```

修改后重启小车服务即可生效。

> **如何标定**：将目标物体（如网球）放在已知距离处，在画面中测量其像素尺寸 `P`，代入公式反算出适合自己场景的 `m` 和 `c` 值。

## 获取标定参数

小车的 `/api/camera/snapshot` 接口在返回图片的同时，会携带当前的标定参数：

```
GET /api/camera/snapshot
```

响应：

```json
{
  "image": "<base64 jpeg>",
  "width": 320,
  "height": 240,
  "format": "jpeg",
  "m": 2671.82,
  "c": -2.82
}
```

| 字段 | 说明 |
|------|------|
| image | 320×240 的 JPEG 图片（letterbox 等比缩放） |
| m | 标定乘数 |
| c | 标定偏移 |

## YOLO 距离仪表盘

项目提供了电脑端仪表盘 `tests/server_dashboard.py`，可从浏览器直观查看检测结果和距离估算。

### 启动

```bash
python tests/server_dashboard.py --model tests/model/tennis.onnx --port 8080
```

浏览器打开 `http://localhost:8080`，输入小车 IP 地址（如 `192.168.4.1`），点击「开始」即可。

### 工作流程

```
小车                         电脑端仪表盘
─────                        ─────────────
/api/camera/snapshot  ←───  拉取图片 + m, c
                             本地 YOLO 检测目标
                             计算 D = m / P + c
                             画框标注，显示距离
```

### 界面说明

| 区域 | 内容 |
|------|------|
| 距离显示 | 实时显示计算出的距离值（cm） |
| 标定参数 | 显示当前使用的 m、c 值 |
| P 值 | 显示检测框的像素尺寸 |
| 图片 | 320×240 画面，检测框以绿色矩形标注 |

### 参数说明

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--model` | `tennis.onnx` | YOLO ONNX 模型路径 |
| `--port` | `8080` | 本机监听端口 |

如果没有 YOLO 模型或模型加载失败，仪表盘仍可运行，仅显示图片和 m/c 参数，不进行检测。