from dataclasses import dataclass
from pathlib import Path

BOARD_CACHE = Path("/run/board_name.cache")
DEFAULT_BOARD = "licheervnano"

# 各板子串口映射 —— lubancat3 的值需按实际引脚复用核对
BOARD_PORTS = {
    "licheervnano": {"arm_port": "/dev/ttyS2", "base_port": "/dev/ttyS1"},
    "lubancat3":    {"arm_port": "/dev/ttyS10", "base_port": "/dev/ttyS3"},
}


def _read_board_name() -> str:
    try:
        name = BOARD_CACHE.read_text().strip().lower()
        if name in BOARD_PORTS:
            return name
    except (FileNotFoundError, OSError):
        pass
    return DEFAULT_BOARD


BOARD_NAME = _read_board_name()
_ports = BOARD_PORTS[BOARD_NAME]


@dataclass(frozen=True)
class HardwareConfig:
    """硬件配置。"""
    arm_driver: str = "zp10s"
    arm_port: str = _ports["arm_port"]
    arm_baudrate: int = 115200

    base_driver: str = "tt_pid"
    base_port: str = _ports["base_port"]

    demo_server_url: str = "http://124.222.162.228:8888"
    ota_check_url: str = "https://api.chenlongrobot.com/api/user/robot-versions/featured"
    status_report_url: str = "https://api.chenlongrobot.com/api/robot-actions"

    # 距离标定: D = m / P + c
    calib_m: float = 2671.82
    calib_c: float = -2.82

config = HardwareConfig()
