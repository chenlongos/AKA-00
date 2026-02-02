import cv2
import os, time
from datetime import datetime
from enum import Enum
import json
import numpy as np
from arm import STS3215, grab, release as arm_release, arm_init
from motor import Motor, forward, backward, turn_left, turn_right, sleep, brake

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
IMAGE_DIR = os.path.abspath(os.path.join(BASE_DIR, '.', 'images'))
OUTPUT_DIR = os.path.join(BASE_DIR, 'output')
OUTPUT_JSON = os.path.join(BASE_DIR, 'output_results.json')
OUTPUT_IMG_DIR = os.path.join(BASE_DIR, 'output', 'images') 
HARDWARE_MODE = 'rk3588'   # cpu, rk3588
if HARDWARE_MODE == 'cpu':
    import onnxruntime as ort
elif HARDWARE_MODE == 'rk3588':
    from rknn.api import RKNN
    rknn = RKNN()
else:
    raise ValueError(f"不支持的硬件模式: {HARDWARE_MODE}")

img_size = 640
CROP_SIZE = 120
fast_speed = 120  # max:240
normal_speed = 30  # max:240
slow_speed = 40  # max:240
scanning_speed = 60  # max:240
max_speed = 240

class RobotStatus(Enum):
    FIND_TENNIS = "find_tennis"
    FIND_BUCKET = "find_bucket"

# 初始化模型
if HARDWARE_MODE == 'cpu':
    MODEL_PATH = os.path.join(BASE_DIR, 'models', 'best.onnx')
    session = ort.InferenceSession(MODEL_PATH, providers=['CPUExecutionProvider'])
    input_name = session.get_inputs()[0].name
elif HARDWARE_MODE == 'rk3588':
    MODEL_PATH = os.path.join(BASE_DIR, 'models', 'best.rknn')
    rknn.load_rknn(MODEL_PATH)
    rknn.init_runtime(target='rk3588')

# 初始化机械臂
servo = STS3215("/dev/ttyACM0", baudrate=115200)
arm_init(servo)

# 初始化车轮
left_motor = Motor(0, 1, 0, chip_type='rk3588')
right_motor = Motor(4, 5, 0, chip_type='rk3588')
sleep(left_motor, right_motor)

def timestamp():
    now = datetime.now()
    return now.strftime("%Y-%m-%d %H:%M:%S") + f".{now.microsecond // 1000:03d}"

def letterbox(img, new_shape=(img_size, img_size), color=(114, 114, 114)):
    """
    YOLOv8 官方预处理函数，保持宽高比 resize + center pad
    """
    shape = img.shape[:2]  # current shape [H, W]
    r = min(new_shape[0] / shape[0], new_shape[1] / shape[1])
    new_unpad = int(round(shape[1] * r)), int(round(shape[0] * r))
    dw, dh = new_shape[1] - new_unpad[0], new_shape[0] - new_unpad[1]
    dw /= 2  # divide padding into 2 sides
    dh /= 2
    if shape[::-1] != new_unpad:
        img = cv2.resize(img, new_unpad, interpolation=cv2.INTER_LINEAR)
    top, bottom = int(round(dh - 0.1)), int(round(dh + 0.1))
    left, right = int(round(dw - 0.1)), int(round(dw + 0.1))
    img = cv2.copyMakeBorder(img, top, bottom, left, right, cv2.BORDER_CONSTANT, value=color)
    return img

def yolo_infer(img_or_frame):
    if isinstance(img_or_frame, str):
        # 输入是图像路径
        orig_img = cv2.imread(img_or_frame)
        if orig_img is None:
            print(f" 无法读取图像: {img_or_frame}")
            return []
    else:
        # 输入是视频帧（ndarray）
        orig_img = img_or_frame

    H, W = orig_img.shape[:2]
    input_img = letterbox(orig_img, new_shape=(img_size, img_size))

    if HARDWARE_MODE == 'cpu':
        blob = cv2.dnn.blobFromImage(input_img, scalefactor=1 / 255.0, size=(img_size, img_size), swapRB=True, crop=False)
        outputs = session.run(None, {input_name: blob})
        pred = outputs[0].squeeze().T  # [C, N] -> [N, C]
    elif HARDWARE_MODE == 'rk3588':
        outputs = rknn.inference(inputs=[input_img])
        pred = outputs[0].squeeze().T  # [C, N] -> [N, C]

    boxes_xywh = pred[:, :4]  # cx, cy, w, h（YOLOv8 输出是 xywh 格式）
    conf_scores = pred[:, 4]
    mask = conf_scores > 0.25

    # dbg:
    # print("ONNX Output Shape:", pred.shape)
    # dbg_max_scores = np.max(pred[:, 4:], axis=1)
    # print("ONNX Max Scores Top 10:", np.sort(dbg_max_scores)[-10:])
    # print(f"Raw output shape: {outputs[0].shape}")
    # print(f"Pred shape after processing: {pred.shape}")
    # print(f"Sample confidence: {conf_scores[:5]}")

    pred = pred[mask]
    boxes_xywh = boxes_xywh[mask]
    conf_scores = conf_scores[mask]

    boxes = []
    raw_boxes = []
    for i in range(len(boxes_xywh)):
        cx, cy, w, h = boxes_xywh[i]
        # letterbox 预处理后，坐标无需复杂的 pad/scale 反推
        # 因为我们知道 letterbox 的 pad 参数，可以精确还原
        shape = orig_img.shape[:2]
        r = min(img_size / shape[0], img_size / shape[1])
        new_unpad = int(round(shape[1] * r)), int(round(shape[0] * r))
        dw, dh = img_size - new_unpad[0], img_size - new_unpad[1]
        dw /= 2
        dh /= 2

        # 将 640x640 坐标还原到缩放后尺寸（new_unpad）
        x1 = (cx - w / 2 - dw) / r
        y1 = (cy - h / 2 - dh) / r
        x2 = (cx + w / 2 - dw) / r
        y2 = (cy + h / 2 - dh) / r

        x1 = max(0, x1)
        y1 = max(0, y1)
        x2 = min(W, x2)
        y2 = min(H, y2)

        raw_boxes.append([x1, y1, x2, y2])

    # NMS
    raw_boxes = np.array(raw_boxes, dtype=np.float32)
    indices = cv2.dnn.NMSBoxes(raw_boxes.tolist(), conf_scores.tolist(), 0.25, 0.45)

    if indices is not None and len(indices) > 0:
        for idx in indices:
            i = int(idx) if np.isscalar(idx) else int(idx[0])
            x1, y1, x2, y2 = raw_boxes[i]
            box = {
                "x": int(x1),
                "y": int(y1),
                "w": int(x2 - x1),
                "h": int(y2 - y1)
            }
            boxes.append(box)

    return boxes

def release():
    if HARDWARE_MODE == 'rk3588':
        rknn.release()
    # 其它硬件无 release 操作

def get_red_bucket_local(frame):
    hsv = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)

    lower_red1 = np.array([0, 80, 50])
    upper_red1 = np.array([10, 255, 255])
    lower_red2 = np.array([170, 80, 50])
    upper_red2 = np.array([180, 255, 255])

    mask = (
            cv2.inRange(hsv, lower_red1, upper_red1)
            | cv2.inRange(hsv, lower_red2, upper_red2)
    )

    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

    boxes = []
    for cnt in contours:
        if cv2.contourArea(cnt) > 5000:
            x, y, w, h = cv2.boundingRect(cnt)
            box = {"x":int(x), "y":int(y), "w":int(w), "h":int(h)}
            boxes.append(box)

    return boxes

def main_v():
    print("正在打开摄像头...")
    cap = cv2.VideoCapture(0)
    if not cap.isOpened():
        raise IOError("无法打开摄像头")
    
    current_status = RobotStatus.FIND_TENNIS
    std_width = 385
    pos_confirm_count = 0

    while True:
        ret, frame = cap.read()
        height, width = frame.shape[:2]

        left_tennis = 440 - CROP_SIZE // 2
        right_tennis = left_tennis + CROP_SIZE
        left_bucket = (width - CROP_SIZE) // 2
        right_bucket = left_bucket + CROP_SIZE

        left_tennis = max(0, left_tennis)
        right_tennis = min(width, right_tennis)
        left_bucket = max(0, left_bucket)
        right_bucket = min(width, right_bucket)

        if current_status == RobotStatus.FIND_TENNIS:
            left = left_tennis
            right = right_tennis
        elif current_status == RobotStatus.FIND_BUCKET:
            left = left_bucket
            right = right_bucket

        try:
            print(f" 解算开始, 时间戳: {timestamp()}")
            if current_status == RobotStatus.FIND_TENNIS:
                result = yolo_infer(frame)
            elif current_status == RobotStatus.FIND_BUCKET:
                result = get_red_bucket_local(frame)
            print(f" 解算完成, 时间戳: {timestamp()}")

            if result and len(result) > 0:
                box = result[0]
                x, y, w, h = box["x"], box["y"], box["w"], box["h"]
                w = max(w, h)
                print("x,y,w,h: ",x,y,w,h)
                center_x = x + w // 2
                if center_x < left :
                    print(" 慢速左转")
                    pos_confirm_count = 0
                    if current_status == RobotStatus.FIND_TENNIS :
                        turn_left(left_motor, right_motor, slow_speed)
                    else:
                        turn_left(left_motor, right_motor, scanning_speed) 
                elif center_x > right :
                    print(" 慢速右转")
                    pos_confirm_count = 0
                    if current_status == RobotStatus.FIND_TENNIS :
                        turn_right(left_motor, right_motor, slow_speed)
                    else:
                        turn_right(left_motor, right_motor, scanning_speed)
                elif w > (std_width * 0.95) and current_status == RobotStatus.FIND_TENNIS :
                    print(" 普速后退")
                    pos_confirm_count = 0
                    backward(left_motor, right_motor, normal_speed)
                elif w < (std_width * 0.85) and current_status == RobotStatus.FIND_TENNIS :
                    pos_confirm_count = 0
                    if w < (std_width * 0.7) :
                        print(" 快速前进")
                        forward(left_motor, right_motor, fast_speed)
                    else :
                        print(" 普速前进")
                        forward(left_motor, right_motor, normal_speed)
                elif w < 640 and current_status == RobotStatus.FIND_BUCKET :
                    print("红桶太远")
                    forward(left_motor, right_motor, max_speed)
                else:
                    if current_status == RobotStatus.FIND_TENNIS:
                        print("抓取网球:", pos_confirm_count )
                        brake(left_motor, right_motor)
                        pos_confirm_count += 1
                        if pos_confirm_count >= 10:
                            grab(servo)
                            time.sleep(2)
                            current_status = RobotStatus.FIND_BUCKET
                    else:
                        print("释放网球")
                        brake(left_motor, right_motor)
                        arm_release(servo)
                        time.sleep(1)
                        backward(left_motor, right_motor, fast_speed)
                        time.sleep(2)
                        current_status = RobotStatus.FIND_TENNIS
            else:
                print(" 目标不在区域内")
                pos_confirm_count = 0
                turn_left(left_motor, right_motor, scanning_speed)

        except Exception as e:
            print(f" 错误处理: {e}")

def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    os.makedirs(OUTPUT_IMG_DIR, exist_ok=True)  
    # ===== 推理并保存结果 =====
    all_results = {}
    all_timings = {}    # 每个图片的推理时间
    total_nums = 0

    for fname in os.listdir(IMAGE_DIR):
        if not fname.lower().endswith(('.jpg', '.jpeg', '.png')):
            continue

        img_path = os.path.join(IMAGE_DIR, fname)
        try:
            start_time = time.time() * 1000
            result = yolo_infer(img_path)
            all_timings[fname] = int(time.time() * 1000 - start_time)
            all_results[fname] = result

            #  读取原图并绘制检测框
            img = cv2.imread(img_path)
            for box in result:
                x, y, w, h = box["x"], box["y"], box["w"], box["h"]
                pt1, pt2 = (x, y), (x + w, y + h)
                cv2.rectangle(img, pt1, pt2, (0, 255, 0), 2)
                cv2.putText(img, "det", (x, y - 5), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1)

            #  保存绘制后的图像
            save_path = os.path.join(OUTPUT_IMG_DIR, fname)
            cv2.imwrite(save_path, img)

            total_nums += len(result)
            print(f" 处理完成: {fname:<20}, 目标数: {len(result)}, 总目标数：{total_nums}")
        except Exception as e:
            print(f" 错误处理 {fname}: {e}")
            all_results[fname] = []

    # ===== 保存为 JSON 文件 =====
    with open(OUTPUT_JSON, 'w', encoding='utf-8') as f:
        json.dump(all_results, f, ensure_ascii=False, indent=2)

    timing_values = list(all_timings.values())
    avg_time = int(sum(timing_values) / len(timing_values) if timing_values else 0)
    max_time = max(timing_values) if timing_values else 0
    min_time = min(timing_values) if timing_values else 0
    print(f" 平均推理时间: {avg_time} ms")
    print(f" 最大推理时间: {max_time} ms")
    print(f" 最小推理时间: {min_time} ms")
    print(f" 总目标数：{total_nums}")
    print(f" 所有结果已保存到: {OUTPUT_JSON}")
    print(f" 检测框图片已保存至: {OUTPUT_IMG_DIR}")

    # 释放资源
    release()

if __name__ == "__main__":
    main_v()
