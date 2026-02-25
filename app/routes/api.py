import math
import os
import base64
import sys
import threading
from pathlib import Path
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
from src.sim.model.car import car
from src.train import train_from_dataset, build_config
from src.policies.act.modeling_act import ACT
from ..extensions import socketio

# 根据AKA_MODE环境变量决定使用硬件还是模拟
# 默认使用模拟模式（避免硬件错误）
aka_mode = os.getenv('AKA_MODE', 'simulation').lower()
use_hardware = aka_mode == 'hardware'

if not use_hardware:
    # 模拟模式 - 使用空实现
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
    # 硬件模式 - 导入真实硬件驱动
    from arm import STS3215, grab, release, arm_init
    from motor import Motor, forward, backward, turn_left, turn_right, sleep, brake

left_motor = Motor(4, 0, 1)
right_motor = Motor(4, 2, 3)

servo = STS3215("/dev/ttyS2", baudrate=115200)
arm_init(servo)

api_bp = Blueprint("api", __name__)
ROOT_DIR = Path(__file__).resolve().parents[2]
OUTPUT_DIR = ROOT_DIR / "output"
DATASET_DIR = OUTPUT_DIR / "datasets"
MODEL_DIR = OUTPUT_DIR / "train"

train_lock = threading.Lock()
train_state = {
    "status": "idle",
    "run_id": None,
    "dataset_path": None,
    "model_id": None,
    "checkpoint_path": None,
    "epoch": 0,
    "num_epochs": 0,
    "avg_loss": None,
    "progress": 0.0,
    "message": None,
    "error": None,
    "started_at": None,
    "ended_at": None,
    "updated_at": None,
}
train_thread = None

infer_lock = threading.Lock()
infer_state = {
    "status": "idle",
    "model_id": None,
    "checkpoint_path": None,
    "device": None,
    "model": None,
    "state_dim": 0,
    "env_state_dim": 0,
    "action_dim": 0,
    "chunk_size": 0,
}


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
        "action_is_pad": torch.tensor(action_is_pad, dtype=torch.bool),
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
                            saved_path = os.path.join(image_dir, saved_name)
                            with open(saved_path, "wb") as f:
                                f.write(raw)
                            image_count += 1
                        except Exception:
                            saved_name = ""
                    if saved_name:
                        step_paths.append(os.path.relpath(saved_path, save_dir))
                    else:
                        step_paths.append("")
                chunk_paths.append(step_paths)
            image_paths.append(chunk_paths)
        dataset[OBS_IMAGES] = image_paths
        if image_count > 0:
            if "meta" not in dataset:
                dataset["meta"] = {}
            dataset["meta"]["image_dir"] = os.path.relpath(image_dir, save_dir)
            dataset["meta"]["image_count"] = image_count
    path = os.path.join(save_dir, f"act_dataset_{timestamp}.pt")
    torch.save(dataset, path)
    return jsonify({"status": "success", "path": path})


def _latest_dataset_path():
    if not DATASET_DIR.exists():
        return None
    candidates = [p for p in DATASET_DIR.glob("*.pt") if p.is_file()]
    if not candidates:
        return None
    candidates.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return str(candidates[0])


def _list_models():
    if not MODEL_DIR.exists():
        return []
    models = []
    for model_dir in MODEL_DIR.iterdir():
        if not model_dir.is_dir():
            continue
        checkpoint_path = model_dir / "act_checkpoint.pt"
        if not checkpoint_path.exists():
            continue
        stat = checkpoint_path.stat()
        models.append(
            {
                "id": model_dir.name,
                "path": str(checkpoint_path),
                "created_at": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(stat.st_ctime)),
                "updated_at": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(stat.st_mtime)),
            }
        )
    models.sort(key=lambda m: m["updated_at"], reverse=True)
    return models


def _map_action(action_vector, action_dim: int):
    if action_dim >= 5:
        idx = int(max(range(5), key=lambda i: action_vector[i]))
        return ["up", "down", "left", "right", "stop"][idx]
    if action_dim == 4:
        idx = int(max(range(4), key=lambda i: action_vector[i]))
        return ["up", "down", "left", "right"][idx]
    if action_dim == 2:
        move = float(action_vector[0])
        turn = float(action_vector[1])
        if abs(move) >= abs(turn):
            return "up" if move >= 0 else "down"
        return "right" if turn >= 0 else "left"
    if action_dim == 1:
        return "up" if float(action_vector[0]) >= 0 else "down"
    return "stop"


def _apply_action(action: str):
    if action == "up":
        if car.speed < car.maxSpeed:
            car.speed += car.acceleration
    if action == "down":
        if car.speed > -car.maxSpeed / 2:
            car.speed -= car.acceleration
    if action == "left":
        car.angle -= car.rotationSpeed
    if action == "right":
        car.angle += car.rotationSpeed

    car.speed *= car.friction
    car.x += math.cos(car.angle) * car.speed
    car.y += math.sin(car.angle) * car.speed

    if action == "stop":
        car.x -= math.cos(car.angle) * car.speed * 2
        car.y -= math.sin(car.angle) * car.speed * 2
        car.speed = 0
    state = car.get_state()
    socketio.emit("car_state", state, broadcast=True)


def _run_training(run_id: str, dataset_path: str, options: dict):
    output_dir = MODEL_DIR / f"act_{time.strftime('%Y%m%d_%H%M%S')}"

    def _progress(epoch: int, total: int, avg_loss: float):
        with train_lock:
            train_state.update(
                {
                    "status": "running",
                    "epoch": epoch,
                    "num_epochs": total,
                    "avg_loss": avg_loss,
                    "progress": float(epoch) / float(total) if total else 0.0,
                    "updated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
                }
            )

    try:
        result = train_from_dataset(
            dataset_path=dataset_path,
            output_dir=str(output_dir),
            num_epochs=options.get("num_epochs", 50),
            batch_size=options.get("batch_size", 64),
            lr=options.get("lr", 3e-4),
            device=options.get("device"),
            use_vae=options.get("use_vae", False),
            kl_weight=options.get("kl_weight", 1.0),
            grad_clip_norm=options.get("grad_clip_norm", 1.0),
            progress_callback=_progress,
        )
        with train_lock:
            train_state.update(
                {
                    "status": "completed",
                    "model_id": output_dir.name,
                    "checkpoint_path": result["checkpoint_path"],
                    "progress": 1.0,
                    "ended_at": time.strftime("%Y-%m-%d %H:%M:%S"),
                    "updated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
                }
            )
    except Exception as exc:
        with train_lock:
            train_state.update(
                {
                    "status": "failed",
                    "error": str(exc),
                    "ended_at": time.strftime("%Y-%m-%d %H:%M:%S"),
                    "updated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
                }
            )


@api_bp.route("/train/start", methods=["POST"])
def train_start():
    data = request.get_json(silent=True) or {}
    dataset_path = data.get("dataset_path") or _latest_dataset_path()
    if not dataset_path or not os.path.exists(dataset_path):
        return jsonify({"status": "error", "message": "dataset not found"}), 400
    with train_lock:
        if train_state["status"] == "running":
            return jsonify({"status": "error", "message": "training already running"}), 400
        run_id = time.strftime("%Y%m%d_%H%M%S")
        train_state.update(
            {
                "status": "starting",
                "run_id": run_id,
                "dataset_path": dataset_path,
                "model_id": None,
                "checkpoint_path": None,
                "epoch": 0,
                "num_epochs": int(data.get("num_epochs", 50)),
                "avg_loss": None,
                "progress": 0.0,
                "message": None,
                "error": None,
                "started_at": time.strftime("%Y-%m-%d %H:%M:%S"),
                "ended_at": None,
                "updated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            }
        )
    options = {
        "num_epochs": data.get("num_epochs", 50),
        "batch_size": data.get("batch_size", 64),
        "lr": data.get("lr", 3e-4),
        "device": data.get("device"),
        "use_vae": data.get("use_vae", False),
        "kl_weight": data.get("kl_weight", 1.0),
        "grad_clip_norm": data.get("grad_clip_norm", 1.0),
    }
    global train_thread
    train_thread = threading.Thread(
        target=_run_training,
        args=(run_id, dataset_path, options),
        daemon=True,
    )
    train_thread.start()
    return jsonify({"status": "started", "run_id": run_id, "dataset_path": dataset_path})


@api_bp.route("/train/status")
def train_status():
    with train_lock:
        return jsonify(dict(train_state))


@api_bp.route("/models")
def models():
    return jsonify({"models": _list_models()})


@api_bp.route("/infer/start", methods=["POST"])
def infer_start():
    data = request.get_json(silent=True) or {}
    model_id = data.get("model_id")
    model_path = data.get("model_path")
    if model_path is None and model_id:
        model_path = str(MODEL_DIR / model_id / "act_checkpoint.pt")
    if not model_path or not os.path.exists(model_path):
        return jsonify({"status": "error", "message": "model not found"}), 400
    device = data.get("device") or ("cuda" if torch.cuda.is_available() else "cpu")
    checkpoint = torch.load(model_path, map_location=device)
    state_dim = int(checkpoint["state_dim"])
    env_state_dim = int(checkpoint["env_state_dim"])
    action_dim = int(checkpoint["action_dim"])
    chunk_size = int(checkpoint["chunk_size"])
    use_vae = bool(checkpoint.get("use_vae", False))
    config = build_config(state_dim, env_state_dim, action_dim, chunk_size, use_vae)
    model = ACT(config)
    model.load_state_dict(checkpoint["model_state_dict"])
    model.to(device)
    model.eval()
    with infer_lock:
        infer_state.update(
            {
                "status": "running",
                "model_id": model_id or Path(model_path).parent.name,
                "checkpoint_path": model_path,
                "device": device,
                "model": model,
                "state_dim": state_dim,
                "env_state_dim": env_state_dim,
                "action_dim": action_dim,
                "chunk_size": chunk_size,
            }
        )
    return jsonify({"status": "started", "model_id": infer_state["model_id"]})


@api_bp.route("/infer/step", methods=["POST"])
def infer_step():
    with infer_lock:
        if infer_state["status"] != "running":
            return jsonify({"status": "error", "message": "inference not running"}), 400
        model = infer_state["model"]
        device = infer_state["device"]
        state_dim = infer_state["state_dim"]
        env_state_dim = infer_state["env_state_dim"]
        action_dim = infer_state["action_dim"]

    state = car.get_state()
    state_vec = [0.0] * state_dim
    state_vec[0:3] = [float(state["x"]), float(state["y"]), float(state["angle"])]
    if state_dim > 3:
        state_vec[3] = float(car.speed)
    state_tensor = torch.tensor([state_vec], dtype=torch.float32, device=device)
    env_tensor = torch.zeros((1, env_state_dim), dtype=torch.float32, device=device)
    with torch.no_grad():
        actions, _ = model({OBS_STATE: state_tensor, OBS_ENV_STATE: env_tensor})
    action_vec = actions[0, 0].detach().cpu().numpy().tolist()
    action = _map_action(action_vec, action_dim)
    _apply_action(action)
    return jsonify({"status": "ok", "action": action, "action_vector": action_vec})


@api_bp.route("/infer/stop", methods=["POST"])
def infer_stop():
    with infer_lock:
        infer_state.update(
            {
                "status": "idle",
                "model_id": None,
                "checkpoint_path": None,
                "device": None,
                "model": None,
                "state_dim": 0,
                "env_state_dim": 0,
                "action_dim": 0,
                "chunk_size": 0,
            }
        )
    return jsonify({"status": "stopped"})


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
