from dataclasses import dataclass


@dataclass(frozen=True)
class HardwareConfig:
    """硬件配置。"""
    arm_driver: str = "zp10s"
    arm_port: str = "/dev/ttyS2"
    arm_baudrate: int = 115200

    base_driver: str = "tt_pid"
    base_chip_type: str = "sg2002"
    base_left_chip: int = 4
    base_right_chip: int = 4


config = HardwareConfig()
