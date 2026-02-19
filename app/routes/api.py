import os
import base64
try:
    import fcntl
    _HAS_FCNTL = True
except Exception:
    _HAS_FCNTL = False
import socket
import struct
import time
import torch

from flask import Blueprint, request, jsonify
from src.utils.constants import OBS_STATE, OBS_ENV_STATE, ACTION, OBS_IMAGE, OBS_IMAGES, REWARD, DONE, TRUNCATED, OBS_LANGUAGE, OBS_LANGUAGE_TOKENS, OBS_LANGUAGE_ATTENTION_MASK, ROBOTS, TELEOPERATORS

if os.name == "nt":
    class STS3215:
        def __init__(self, *_, **__):
            pass

    def grab(_):
        return None

    def release(_):
        return None

    def arm_init(_):
        return None

    class Motor:
        def __init__(self, *_, **__):
            pass

    def forward(*_, **__):
        return None

    def backward(*_, **__):
        return None

    def turn_left(*_, **__):
        return None

    def turn_right(*_, **__):
        return None

    def sleep(*_, **__):
        return None

    def brake(*_, **__):
        return None
else:
    from arm import STS3215, grab, release, arm_init
    from motor import Motor, forward, backward, turn_left, turn_right, sleep, brake

left_motor = Motor(4, 0, 1)
right_motor = Motor(4, 2, 3)

servo = STS3215("/dev/ttyS2", baudrate=115200)
arm_init(servo)

api_bp = Blueprint("api", __name__)


@api_bp.route("/ip")
def ip():
    return jsonify({
        "ip": get_ip()
    })


@api_bp.route('/control', methods=['GET'])
def control():
    action = request.args.get('action')
    speed = int(request.args.get('speed', 50))
    milliseconds = float(request.args.get('time', 0))

    speed = speed * 240 // 50
    # --- 运动逻辑 ---
    if action == 'up':
        # print('up')
        forward(left_motor, right_motor, speed)
    elif action == 'down':
        # print('down')
        backward(left_motor, right_motor, speed)
    elif action == 'left':
        # print('left')
        turn_left(left_motor, right_motor, speed)
    elif action == 'right':
        # print('right')
        turn_right(left_motor, right_motor, speed)
    elif action == 'stop':
        # print('stop')
        brake(left_motor, right_motor)
    elif action == 'grab':
        # print('grab')
        grab(servo)
    elif action == 'release':
        # print('release')
        release(servo)

    if milliseconds > 0 and action in ['up', 'down', 'left', 'right']:
        time.sleep(milliseconds / 1000.0)
        # sleep(left_motor, right_motor)

        return jsonify({"status": "success", "message": f"{action} for {milliseconds}s done"})

    return jsonify({"status": "success", "action": action})


@api_bp.route('/dataset', methods=['POST'])
def save_dataset():
    payload = request.get_json(silent=True) or {}
    states = payload.get("states") or payload.get(OBS_STATE)
    env_states = payload.get("env_states") or payload.get(OBS_ENV_STATE)
    actions = payload.get("actions") or payload.get(ACTION)
    action_is_pad = payload.get("action_is_pad")
    rewards = payload.get(REWARD)
    dones = payload.get(DONE)
    truncateds = payload.get(TRUNCATED)
    languages = payload.get(OBS_LANGUAGE)
    language_tokens = payload.get(OBS_LANGUAGE_TOKENS)
    language_masks = payload.get(OBS_LANGUAGE_ATTENTION_MASK)
    robots = payload.get(ROBOTS)
    teleoperators = payload.get(TELEOPERATORS)
    images = payload.get(OBS_IMAGES)
    if images is None:
        single_images = payload.get(OBS_IMAGE)
        if single_images is not None:
            images = [[[(img if img is not None else "")] for img in chunk] for chunk in single_images]
    if images is None:
        legacy_images = payload.get("images")
        if legacy_images is not None:
            images = [[[(img if img is not None else "")] for img in chunk] for chunk in legacy_images]
    if not (isinstance(states, list) and isinstance(env_states, list) and isinstance(actions, list) and isinstance(action_is_pad, list)):
        return jsonify({"error": "invalid payload"}), 400
    if not (len(states) == len(env_states) == len(actions) == len(action_is_pad)):
        return jsonify({"error": "length mismatch"}), 400
    if rewards is not None and (not isinstance(rewards, list) or len(rewards) != len(actions)):
        return jsonify({"error": "reward length mismatch"}), 400
    if dones is not None and (not isinstance(dones, list) or len(dones) != len(actions)):
        return jsonify({"error": "done length mismatch"}), 400
    if truncateds is not None and (not isinstance(truncateds, list) or len(truncateds) != len(actions)):
        return jsonify({"error": "truncated length mismatch"}), 400
    if languages is not None and (not isinstance(languages, list) or len(languages) != len(actions)):
        return jsonify({"error": "language length mismatch"}), 400
    if language_tokens is not None and (not isinstance(language_tokens, list) or len(language_tokens) != len(actions)):
        return jsonify({"error": "language tokens length mismatch"}), 400
    if language_masks is not None and (not isinstance(language_masks, list) or len(language_masks) != len(actions)):
        return jsonify({"error": "language masks length mismatch"}), 400
    if images is not None:
        if not isinstance(images, list):
            return jsonify({"error": "invalid images"}), 400
        if len(images) != len(actions):
            return jsonify({"error": "images length mismatch"}), 400
    dataset = {
        OBS_STATE: torch.tensor(states, dtype=torch.float32),
        OBS_ENV_STATE: torch.tensor(env_states, dtype=torch.float32),
        ACTION: torch.tensor(actions, dtype=torch.float32),
        "action_is_pad": torch.tensor(action_is_pad, dtype=torch.float32),
    }
    if rewards is not None:
        dataset[REWARD] = torch.tensor(rewards, dtype=torch.float32)
    if dones is not None:
        dataset[DONE] = torch.tensor(dones, dtype=torch.bool)
    if truncateds is not None:
        dataset[TRUNCATED] = torch.tensor(truncateds, dtype=torch.bool)
    if languages is not None:
        dataset[OBS_LANGUAGE] = languages
    if language_tokens is not None:
        dataset[OBS_LANGUAGE_TOKENS] = language_tokens
    if language_masks is not None:
        dataset[OBS_LANGUAGE_ATTENTION_MASK] = language_masks
    if isinstance(robots, list):
        dataset[ROBOTS] = robots
    if isinstance(teleoperators, list):
        dataset[TELEOPERATORS] = teleoperators
    meta = payload.get("meta")
    if isinstance(meta, dict):
        dataset["meta"] = meta
    save_dir = os.getenv("ACT_DATASET_DIR", os.path.join("output", "datasets"))
    os.makedirs(save_dir, exist_ok=True)
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    image_dir = None
    image_count = 0
    image_paths = None
    if images is not None:
        image_dir = os.path.join(save_dir, f"images_{timestamp}")
        os.makedirs(image_dir, exist_ok=True)
        image_paths = []
        for chunk_index, chunk in enumerate(images):
            if not isinstance(chunk, list):
                image_paths.append([])
                continue
            chunk_paths = []
            for step_index, camera_list in enumerate(chunk):
                if not isinstance(camera_list, list):
                    camera_list = [camera_list]
                step_paths = []
                for cam_index, data_url in enumerate(camera_list):
                    saved_name = ""
                    if isinstance(data_url, str) and data_url and "base64," in data_url:
                        _, b64 = data_url.split("base64,", 1)
                        try:
                            raw = base64.b64decode(b64)
                            saved_name = f"chunk_{chunk_index:05d}_step_{step_index:02d}_cam_{cam_index}.png"
                            with open(os.path.join(image_dir, saved_name), "wb") as f:
                                f.write(raw)
                            image_count += 1
                        except Exception:
                            saved_name = ""
                    step_paths.append(saved_name)
                chunk_paths.append(step_paths)
            image_paths.append(chunk_paths)
        dataset[OBS_IMAGES] = image_paths
        if image_count > 0:
            if "meta" not in dataset:
                dataset["meta"] = {}
            dataset["meta"]["image_dir"] = image_dir
            dataset["meta"]["image_count"] = image_count
    path = os.path.join(save_dir, f"act_dataset_{timestamp}.pt")
    torch.save(dataset, path)
    return jsonify({"status": "success", "path": path})


def get_ip(ifname="wlan0"):
    if not _HAS_FCNTL:
        return socket.gethostbyname(socket.gethostname())
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    return socket.inet_ntoa(
        fcntl.ioctl(
            s.fileno(),
            0x8915,
            struct.pack('256s', ifname[:15].encode('utf-8'))
        )[20:24]
    )
