// csrc/angle_config.hpp — 机械臂角度配置（对应 src/arm_control/angle_config.py）
//
// 语义化结构（新格式）:
//   {
//     "grab_position": {"servo0": 245, "servo1": 180},
//     "lift_position": {"servo0": 200, "servo1": 180},
//     "gripper_open": 150,
//     "gripper_close": 90
//   }
//
// 文件位置：$ARM_ANGLES_PATH > $AKA_HOME/arm_angles.json > cwd/arm_angles.json

#pragma once

#include <string>

#include "csrc/json.hpp"

namespace csrc {

inline constexpr const char* ZP10S_DRIVER = "zp10s";
inline constexpr const char* STS3215_DRIVER = "sts3215";

/// 角度配置文件路径（ARM_ANGLES_PATH > cwd/arm_angles.json）
std::string arm_angles_path();

/// 加载角度配置（自动迁移旧格式、用默认值填充缺失字段）。返回语义化 Json。
Json load_arm_angles(const std::string& driver);

/// 保存角度配置（自动标准化），返回标准化后的 Json。
Json save_arm_angles(const std::string& driver, const Json& angles);

// ── 便捷访问 ──
int get_grab_servo(const Json& angles, const std::string& servo_key, const std::string& driver = ZP10S_DRIVER);
int get_lift_servo(const Json& angles, const std::string& servo_key, const std::string& driver = ZP10S_DRIVER);
int get_gripper_open(const Json& angles, const std::string& driver = ZP10S_DRIVER);
int get_gripper_close(const Json& angles, const std::string& driver = ZP10S_DRIVER);

/// 默认角度（不带文件，纯默认值，迁移/测试用）
Json default_arm_angles(const std::string& driver);

/// 夹爪舵机 ID：ZP10S=2, STS3215=3
inline int gripper_servo_id(const std::string& driver) {
    return driver == STS3215_DRIVER ? 3 : 2;
}

}  // namespace csrc
