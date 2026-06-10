"""电脑端 server + 前端：从小车拉图片 + m/c，本地 YOLO 检测 + 距离估算 D = m/P + c

启动:
    python tests/server_dashboard.py --model models/best.onnx

浏览器打开 http://localhost:8080 ，输入小车 IP，点开始
"""
import argparse
import base64
import json
import urllib.request
import cv2
import numpy as np
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

IMG_SIZE = 640
CONF = 0.25
IOU = 0.45

# ---- 全局模型 ----
session = None
input_name = None


def load_model(model_path):
    global session, input_name
    import onnxruntime as ort
    session = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
    input_name = session.get_inputs()[0].name
    print(f"Model loaded: {model_path}")


def letterbox(img, new_shape=(IMG_SIZE, IMG_SIZE), color=(114, 114, 114)):
    shape = img.shape[:2]
    r = min(new_shape[0] / shape[0], new_shape[1] / shape[1])
    new_unpad = int(round(shape[1] * r)), int(round(shape[0] * r))
    dw = (new_shape[1] - new_unpad[0]) / 2
    dh = (new_shape[0] - new_unpad[1]) / 2
    if shape[::-1] != new_unpad:
        img = cv2.resize(img, new_unpad, interpolation=cv2.INTER_LINEAR)
    top, bottom = int(round(dh - 0.1)), int(round(dh + 0.1))
    left, right = int(round(dw - 0.1)), int(round(dw + 0.1))
    return cv2.copyMakeBorder(img, top, bottom, left, right, cv2.BORDER_CONSTANT, value=color)


def yolo_detect(frame):
    """YOLOv8 检测 → 框列表 [{x,y,w,h}, ...] 按宽度降序"""
    H, W = frame.shape[:2]
    input_img = letterbox(frame)
    blob = cv2.dnn.blobFromImage(input_img, 1 / 255.0, (IMG_SIZE, IMG_SIZE), swapRB=True, crop=False)
    outputs = session.run(None, {input_name: blob})
    pred = outputs[0].squeeze().T  # [N, C]

    boxes_xywh = pred[:, :4]
    conf_scores = pred[:, 4]
    mask = conf_scores > CONF
    pred = pred[mask]
    boxes_xywh = boxes_xywh[mask]
    conf_scores = conf_scores[mask]

    # letterbox 参数
    r = min(IMG_SIZE / H, IMG_SIZE / W)
    new_unpad = int(round(W * r)), int(round(H * r))
    dw = (IMG_SIZE - new_unpad[0]) / 2
    dh = (IMG_SIZE - new_unpad[1]) / 2

    raw_boxes = []
    for i in range(len(boxes_xywh)):
        cx, cy, w, h = boxes_xywh[i]
        x1 = (cx - w / 2 - dw) / r
        y1 = (cy - h / 2 - dh) / r
        x2 = (cx + w / 2 - dw) / r
        y2 = (cy + h / 2 - dh) / r
        raw_boxes.append([max(0, x1), max(0, y1), min(W, x2), min(H, y2)])

    if not raw_boxes:
        return []

    raw_boxes = np.array(raw_boxes, dtype=np.float32)
    indices = cv2.dnn.NMSBoxes(raw_boxes.tolist(), conf_scores.tolist(), CONF, IOU)
    if indices is None or len(indices) == 0:
        return []

    boxes = []
    for idx in indices:
        i = int(idx) if np.isscalar(idx) else int(idx[0])
        x1, y1, x2, y2 = raw_boxes[i]
        boxes.append({"x": int(x1), "y": int(y1), "w": int(x2 - x1), "h": int(y2 - y1)})

    boxes.sort(key=lambda b: b["w"], reverse=True)
    return boxes


HTML = r"""<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>小车摄像头</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:system-ui,sans-serif;background:#111;color:#eee;display:flex;flex-direction:column;align-items:center;padding:16px;min-height:100vh}
h1{font-size:18px;margin-bottom:12px}
.card{background:#1a1a1a;border-radius:12px;padding:16px;width:100%;max-width:400px;margin-bottom:12px}
.row{display:flex;gap:12px;align-items:center;flex-wrap:wrap}
input{background:#222;color:#fff;border:1px solid #333;border-radius:6px;padding:6px 10px;font-size:14px}
button{background:#2563eb;color:#fff;border:none;border-radius:6px;padding:8px 16px;font-size:13px;cursor:pointer}
button:active{opacity:.8}
img{width:320px;max-width:100%;border-radius:8px;margin-top:8px;image-rendering:crisp-edges}
.badge{display:inline-block;background:#2563eb;color:#fff;border-radius:4px;padding:2px 8px;font-size:12px;margin:2px}
.mono{font-family:monospace;font-size:13px}
.value{font-size:28px;font-weight:700;color:#60a5fa}
.unit{font-size:14px;color:#888}
.status{font-size:11px;color:#666;margin-top:4px}
.err{color:#f87171;font-size:10px}
</style>
</head>
<body>
<h1>小车摄像头</h1>

<div class="card">
  <div class="row">
    <input id="carAddr" placeholder="小车 IP，如 192.168.4.1" style="width:400px">
    <button onclick="togglePoll()" id="btnToggle">开始</button>
  </div>
  <div class="status" id="carStatus">输入 IP 后点开始</div>
</div>

<div class="card">
  <div class="row">
    <div>
      <div class="value" id="dist">--</div>
      <div class="unit">cm</div>
    </div>
    <div style="margin-left:auto;text-align:right">
      <div><span class="badge">m</span> <span class="mono" id="valM">--</span></div>
      <div><span class="badge">c</span> <span class="mono" id="valC">--</span></div>
      <div style="margin-top:4px"><span class="mono" id="valP">P=--</span></div>
    </div>
  </div>
  <img id="img" src="" alt="等待连接...">
  <div class="err" id="errMsg"></div>
</div>

<script>
let running = false, timer = null;

function togglePoll() {
  const addr = document.getElementById('carAddr').value.trim();
  if (!running && !addr) {
    document.getElementById('carStatus').textContent = '请先输入小车 IP';
    document.getElementById('carStatus').style.color = '#f87171';
    return;
  }
  running = !running;
  document.getElementById('btnToggle').textContent = running ? '停止' : '开始';
  if (running) { poll(); timer = setInterval(poll, 300); }
  else { clearInterval(timer); }
}

async function poll() {
  const addr = document.getElementById('carAddr').value.trim();
  if (!addr) return;
  try {
    const r = await fetch('/api/dashboard?car=' + encodeURIComponent(addr));
    const d = await r.json();
    if (d.error) { showErr(d.error); return; }
    showErr('');
    document.getElementById('img').src = 'data:image/jpeg;base64,' + d.image;
    document.getElementById('valM').textContent = d.m;
    document.getElementById('valC').textContent = d.c;
    document.getElementById('valP').textContent = 'P=' + (d.pixel_width || '--');
    document.getElementById('dist').textContent = d.distance_cm != null ? d.distance_cm : '--';
    document.getElementById('carStatus').textContent = '已连接';
    document.getElementById('carStatus').style.color = '#4ade80';
  } catch (e) {
    showErr(e.message);
    document.getElementById('carStatus').textContent = '连接失败';
    document.getElementById('carStatus').style.color = '#f87171';
  }
}

function showErr(msg) {
  document.getElementById('errMsg').textContent = msg;
}
</script>
</body>
</html>"""


class Handler(BaseHTTPRequestHandler):
    def _json(self, data, status=200):
        body = json.dumps(data, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)

        if parsed.path == "/" or parsed.path == "/index.html":
            body = HTML.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        elif parsed.path == "/api/dashboard":
            params = parse_qs(parsed.query)
            car = params.get("car", [None])[0]
            if not car:
                self._json({"error": "missing ?car=IP"}, 400)
                return

            # 从车拉 snapshot
            car_url = f"http://{car}/api/camera/snapshot"
            try:
                with urllib.request.urlopen(car_url, timeout=2) as resp:
                    data = json.loads(resp.read())
            except Exception as e:
                self._json({"error": str(e)}, 500)
                return

            if "error" in data:
                self._json(data, 500)
                return

            m = data.get("m", 0)
            c = data.get("c", 0)

            # 解码图片 → YOLO 检测 → 算距离 → 画框
            img_bytes = base64.b64decode(data["image"])
            frame = cv2.imdecode(np.frombuffer(img_bytes, np.uint8), cv2.IMREAD_COLOR)

            if frame is not None and session is not None:
                boxes = yolo_detect(frame)
                box = boxes[0] if boxes else None
            else:
                box = None

            if box:
                P = max(box["w"], box["h"])
                D = (m / P + c) if P > 0 else None
                # 在 320x240 原图上画第一个框
                x, y, w, h = box["x"], box["y"], box["w"], box["h"]
                cv2.rectangle(frame, (x, y), (x + w, y + h), (0, 255, 0), 2)
                label = f"{D:.0f}cm P={P}"
                cv2.putText(frame, label, (x, y - 6), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 255, 0), 2)
                _, jpg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
                image_out = base64.b64encode(jpg.tobytes()).decode("ascii")
            else:
                P = 0
                D = None
                image_out = data["image"]

            self._json({
                "image": image_out,
                "width": 320,
                "height": 240,
                "m": m,
                "c": c,
                "pixel_width": P,
                "distance_cm": round(D, 1) if D else None,
                "box": box,
            })

        else:
            self._json({"error": "not found"}, 404)

    def log_message(self, fmt, *args):
        print(f"[{self.log_date_time_string()}] {args[0]}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="tennis.onnx", help="YOLO ONNX 模型路径")
    parser.add_argument("--port", type=int, default=8080, help="本机监听端口")
    args = parser.parse_args()

    try:
        load_model(args.model)
    except Exception as e:
        print(f"模型加载失败: {e}")
        print("将跳过 YOLO 检测，只显示图片和 m/c")
        # 不退出，没有模型也能用

    server = HTTPServer(("0.0.0.0", args.port), Handler)
    print(f"仪表盘: http://localhost:{args.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n关闭...")
        server.shutdown()


if __name__ == "__main__":
    main()
