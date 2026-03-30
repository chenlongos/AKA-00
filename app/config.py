import os
from dataclasses import dataclass


@dataclass(frozen=True)
class HardwareConfig:
    arm_driver: str = "zp10s"
    arm_port: str = "/dev/ttyS2"
    arm_baudrate: int = 115200
    base_chip_type: str = "sg2002"
    base_left_chip: int = 4
    base_left_ch1: int = 0
    base_left_ch2: int = 1
    base_right_chip: int = 4
    base_right_ch1: int = 2
    base_right_ch2: int = 3
    wifi_interface: str = "wlan0"


def load_hardware_config() -> HardwareConfig:
    return HardwareConfig(
        arm_driver=os.getenv("AKA_ARM_DRIVER", "zp10s").lower(),
        arm_port=os.getenv("AKA_ARM_PORT", "/dev/ttyS2"),
        arm_baudrate=int(os.getenv("AKA_ARM_BAUDRATE", "115200")),
        base_chip_type=os.getenv("AKA_BASE_CHIP_TYPE", "sg2002"),
        base_left_chip=int(os.getenv("AKA_BASE_LEFT_CHIP", "4")),
        base_left_ch1=int(os.getenv("AKA_BASE_LEFT_CH1", "0")),
        base_left_ch2=int(os.getenv("AKA_BASE_LEFT_CH2", "1")),
        base_right_chip=int(os.getenv("AKA_BASE_RIGHT_CHIP", "4")),
        base_right_ch1=int(os.getenv("AKA_BASE_RIGHT_CH1", "2")),
        base_right_ch2=int(os.getenv("AKA_BASE_RIGHT_CH2", "3")),
        wifi_interface=os.getenv("AKA_WIFI_INTERFACE", "wlan0"),
    )
