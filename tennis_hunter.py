import cv2
import os, time, logging
from datetime import datetime
import json
import numpy as np
from dataclasses import dataclass, field
from arm import STS3215, grab1, grab_pos, release as arm_release, release_pos, arm_init
from motor import Motor, forward, backward, turn_left, turn_right,forward_left, forward_right, sleep as motor_sleep, brake

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
IMAGE_DIR = os.path.abspath(os.path.join(BASE_DIR, '.', 'images'))
OUTPUT_DIR = os.path.join(BASE_DIR, 'output')
OUTPUT_JSON = os.path.join(BASE_DIR, 'output_results.json')
OUTPUT_IMG_DIR = os.path.join(BASE_DIR, 'output', 'images') 
HARDWARE_MODE = 'rk3588'   # cpu, rk3588

# 日志级别
logging.basicConfig(
    level = logging.INFO, 
    format='%(asctime)s - %(levelname)s - %(message)s'
)

if HARDWARE_MODE == 'cpu':
    import onnxruntime as ort
elif HARDWARE_MODE == 'rk3588':
    from rknn.api import RKNN
    rknn = RKNN()
else:
    raise ValueError(f"不支持的硬件模式: {HARDWARE_MODE}")

# 初始化模型
if HARDWARE_MODE == 'cpu':
    MODEL_PATH = os.path.join(BASE_DIR, 'models', 'best.onnx')
    session = ort.InferenceSession(MODEL_PATH, providers=['CPUExecutionProvider'])
    input_name = session.get_inputs()[0].name
elif HARDWARE_MODE == 'rk3588':
    MODEL_PATH = os.path.join(BASE_DIR, 'models', 'best.rknn')
    rknn.load_rknn(MODEL_PATH)
    rknn.init_runtime(target='rk3588')

def letterbox(img, new_shape=(640, 640), color=(114, 114, 114)):
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
    img_size = 640

    if isinstance(img_or_frame, str):
        # 输入是图像路径
        orig_img = cv2.imread(img_or_frame)
        if orig_img is None:
            logging.warning(f" 无法读取图像: {img_or_frame}")
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
    # logging.debug(f"ONNX Output Shape: {pred.shape})
    # dbg_max_scores = np.max(pred[:, 4:], axis=1)
    # logging.debug(f"ONNX Max Scores Top 10: {np.sort(dbg_max_scores)[-10:]})
    # logging.debug(f"Raw output shape: {outputs[0].shape}")
    # logging.debug(f"Pred shape after processing: {pred.shape}")
    # logging.debug(f"Sample confidence: {conf_scores[:5]}")

    pred = pred[mask]
    boxes_xywh = boxes_xywh[mask]
    conf_scores = conf_scores[mask]

    boxes = []
    raw_boxes = []
    for i in range(len(boxes_xywh)):
        cx, cy, w, h = boxes_xywh[i]
        #  使用letterbox 的 pad 参数精确还原
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

def show_box(frame, result):
    if HARDWARE_MODE != "cpu":
        return
    
    for box in result:
        x, y, w, h = box.x, box.y, box.w, box.h
        center_x = x + w // 2
        center_y = y + h // 2
        pt1, pt2 = (x, y), (x + w, y + h)
        cv2.rectangle(frame, pt1, pt2, (0, 255, 0), 2)
        cv2.circle(frame, (center_x, center_y), 5, (0, 0, 255), -1)

    cv2.imshow("frame", frame)

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

@dataclass
class Robot:
    FRAME_WIDTH = 640
    X_LEFT_GRAB = 258
    X_RIGHT_GRAB = X_LEFT_GRAB + 40
    TENNIS_WIDTH_FAR = 300
    TENNIS_WIDTH_NEAR = 360
    MAX_SPEED = 240
    MIN_SPEED = MAX_SPEED // 6  # 40
    status: str = "chase_tennis" # 机器人状态: chase_tennis, chase_bucket, grab_tennis, position_tennis, release_tennis
    box_cur_width: int = 0
    box_cur_height: int = 0
    box_cur_x: int = 0
    frame_height: int = 0

    idle_speed = MAX_SPEED // 3  # 80
    grab_confirm_count = 0

    servo: STS3215 = field(init=False)
    left_motor: Motor = field(init=False)
    right_motor: Motor = field(init=False)

    def __post_init__(self):
        # 初始化机械臂
        self.servo = STS3215("/dev/ttyACM0", baudrate=115200)
        arm_init(self.servo)
        grab_pos(self.servo)

        # 初始化车轮
        self.left_motor = Motor(0, 1, 0, chip_type='rk3588')
        self.right_motor = Motor(4, 5, 0, chip_type='rk3588')
        motor_sleep(self.left_motor, self.right_motor)

    def update_status(self):
        if self.status == "chase_tennis":
            if self.TENNIS_WIDTH_FAR <= self.box_cur_width <= self.TENNIS_WIDTH_NEAR:
                self.status = "position_tennis"
        elif self.status == "position_tennis":
            if not self.TENNIS_WIDTH_FAR < self.box_cur_width < self.TENNIS_WIDTH_NEAR:
                self.status = "chase_tennis"
            elif self.X_LEFT_GRAB <= self.box_cur_x <= self.X_RIGHT_GRAB:
                self.status = "grab_tennis"
        elif self.status == "chase_bucket" and self.box_cur_width >= self.FRAME_WIDTH:
            self.status = "release_tennis"

    def set_motor_speed(self, result):
        IMG_WIDTH = self.FRAME_WIDTH
        MAX_SPEED = self.MAX_SPEED
        MIN_SPEED = self.MIN_SPEED
        WHEEL_BASE = 10.0
        TARGET_X = IMG_WIDTH // 2
        if self.status == "chase_bucket":
            TARGET_W = IMG_WIDTH
        else:
            TARGET_W = int(self.TENNIS_WIDTH_FAR * 0.6 + self.TENNIS_WIDTH_NEAR * 0.4)

        Kp_dist = 1.0 if self.status == "chase_bucket" else 0.8
        Kp_angle = 0.04 if self.status == "chase_bucket" else 0.02

        result_sorted = sorted(result, key=lambda x: x['w'], reverse=True)
        box = result_sorted[0]
        x, w, h = box["x"], box["w"], box["h"]
        self.box_cur_height = h
        self.box_cur_x = x
        self.box_cur_width = w
        logging.info("(box_cur_x, box_cur_width, box_cur_height) ==> %d, %d, %d", self.box_cur_x, self.box_cur_width, self.box_cur_height)

        # 1. 计算偏差
        error_x = (x + w / 2) - TARGET_X
        error_w = w - TARGET_W

        # 2. 计算线性速度和角速度
        raw_v = -Kp_dist * error_w
        raw_omega = -Kp_angle * error_x

        # 3. 动态限速
        turn_factor = abs(error_x) / (IMG_WIDTH / 2)
        if turn_factor > 0.8:
            max_v = MIN_SPEED * 0.3
        else:
            max_v = MAX_SPEED

        v = max(min(raw_v, max_v), -max_v)

        if abs(v) < MIN_SPEED and abs(v) > 0:
            v = MIN_SPEED if v > 0 else -MIN_SPEED

        diff_speed = raw_omega * WHEEL_BASE

        # 4. 差速解算
        left_pwm = v + diff_speed
        right_pwm = v - diff_speed

        # 5. 限速
        left_pwm = max(-MAX_SPEED, min(MAX_SPEED, left_pwm))
        right_pwm = max(-MAX_SPEED, min(MAX_SPEED, right_pwm))

        if abs(left_pwm) < MIN_SPEED and left_pwm != 0:
            left_pwm = MIN_SPEED if left_pwm > 0 else -MIN_SPEED
        if abs(right_pwm) < MIN_SPEED and right_pwm != 0:
            right_pwm = MIN_SPEED if right_pwm > 0 else -MIN_SPEED

        return int(left_pwm), int(right_pwm)

    def motor_move(self, left_speed, right_speed):
        self.grab_confirm_count = 0
        if self.status == "position_tennis":
            if self.box_cur_x < self.X_LEFT_GRAB:
                turn_left(self.left_motor, self.right_motor, self.MIN_SPEED)
            elif self.box_cur_x > self.X_RIGHT_GRAB:
                turn_right(self.left_motor, self.right_motor, self.MIN_SPEED)
            return
        # bucket is too close
        if self.status == "chase_bucket" and self.box_cur_height == self.frame_height:
            if self.box_cur_x == 0:
                # bucket on the left
                if self.box_cur_width <self.FRAME_WIDTH:    
                    turn_left(self.left_motor, self.right_motor, self.idle_speed)
                # don't know bucket position, need to backward
                else:
                    backward(self.left_motor, self.right_motor, self.idle_speed)
                return
            else:
                # bucket on the right
                if self.box_cur_x + self.box_cur_width == self.FRAME_WIDTH:
                    turn_right(self.left_motor, self.right_motor, self.idle_speed)
                    return
                # here bucket in the middle

        self.left_motor.set_speed(left_speed)
        self.right_motor.set_speed(right_speed)
    
    def grab_tennis(self):
        if self.status == "grab_tennis":
            brake(self.left_motor, self.right_motor)
            self.grab_confirm_count += 1
            if self.grab_confirm_count >= 10:
                grab1(self.servo)
                self.grab_confirm_count = 0
                time.sleep(0.5)
                pos_servo3 = self.servo.get_position(3)
                if pos_servo3 == None or pos_servo3 < 3050:
                    grab_pos(self.servo)
                    self.status = "chase_tennis"
                    return
                backward(self.left_motor, self.right_motor, self.MAX_SPEED)
                time.sleep(0.4)
                self.status = "chase_bucket"
                release_pos(self.servo)

    def release_tennis(self):
        if self.status == "release_tennis":
            forward(self.left_motor, self.right_motor, self.MAX_SPEED)
            time.sleep(0.5)
            motor_sleep(self.left_motor, self.right_motor)
            arm_release(self.servo)
            time.sleep(0.5)
            backward(self.left_motor, self.right_motor, self.MAX_SPEED)
            time.sleep(0.5)
            self.status = "chase_tennis"
            grab_pos(self.servo)

    def idle(self):
        self.grab_confirm_count = 0
        turn_right(self.left_motor, self.right_motor, self.idle_speed)

def main_v():
    all_timings = []    # 每帧的处理时间
    
    logging.info("正在打开摄像头...")
    cap = cv2.VideoCapture(0)
    if not cap.isOpened():
        raise IOError("无法打开摄像头")
    
    robot = Robot()

    while True:
        start_time = time.time() * 1000
        _ret, frame = cap.read()
        height, width = frame.shape[:2]
        robot.frame_height = height
        if width != robot.FRAME_WIDTH:
            logging.warning(f" 当前帧宽度 {width} 不等于 {robot.FRAME_WIDTH}，正在调整大小...")
            frame = cv2.resize(frame, (robot.FRAME_WIDTH, int(robot.FRAME_WIDTH * height / width)), interpolation=cv2.INTER_LINEAR)
            robot.frame_height = frame.shape[0]

        logging.debug(f" 解算开始")
        if robot.status == "chase_bucket":
            result = get_red_bucket_local(frame)
        else:
            result = yolo_infer(frame)
        logging.debug(f" 解算完成")

        if result:
            robot.update_status()
            logging.info(f"update_status: status ==> {robot.status}")
            left_speed, right_speed = robot.set_motor_speed(result)
            logging.info(f"set_motor_speed: (left_speed, right_speed) ==> {left_speed}, {right_speed}")
        else:
            logging.info("idle")
            robot.idle()
            continue

        if robot.status == "grab_tennis":
            robot.grab_tennis()
        elif robot.status == "release_tennis":
            robot.release_tennis()
        else:
            # robot.move()
            robot.motor_move(left_speed, right_speed)
        
        all_timings.append(int(time.time() * 1000 - start_time))
        avg_time = int(sum(all_timings) / len(all_timings) if all_timings else 0)
        min_time = min(all_timings) if all_timings else 0
        max_time = max(all_timings) if all_timings else 0
        logging.info(f"当前帧处理时间: {all_timings[-1]} ms, 平均: {avg_time} ms, 最小: {min_time} ms, 最大: {max_time} ms")

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
            logging.info(f" 处理完成: {fname:<20}, 目标数: {len(result)}, 总目标数：{total_nums}")
        except Exception as e:
            logging.error(f" 错误处理 {fname}: {e}")
            all_results[fname] = []

    # ===== 保存为 JSON 文件 =====
    with open(OUTPUT_JSON, 'w', encoding='utf-8') as f:
        json.dump(all_results, f, ensure_ascii=False, indent=2)

    timing_values = list(all_timings.values())
    avg_time = int(sum(timing_values) / len(timing_values) if timing_values else 0)
    max_time = max(timing_values) if timing_values else 0
    min_time = min(timing_values) if timing_values else 0
    logging.info(f" 平均推理时间: {avg_time} ms")
    logging.info(f" 最大推理时间: {max_time} ms")
    logging.info(f" 最小推理时间: {min_time} ms")
    logging.info(f" 总目标数：{total_nums}")
    logging.info(f" 所有结果已保存到: {OUTPUT_JSON}")
    logging.info(f" 检测框图片已保存至: {OUTPUT_IMG_DIR}")

    # 释放资源
    release()

if __name__ == "__main__":
    main_v()
