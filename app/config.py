from dataclasses import dataclass


@dataclass(frozen=True)
class HardwareConfig:
    """硬件配置。"""
    arm_driver: str = "zp10s"
    arm_port: str = "/dev/ttyS2"
    arm_baudrate: int = 115200

    base_driver: str = "tt_pid"
    base_port: str = "/dev/ttyS1"

    demo_server_url: str = "http://124.222.162.228:8888"

    # 距离标定: D = m / P + c
    calib_m: float = 2671.82
    calib_c: float = -2.82

config = HardwareConfig()
