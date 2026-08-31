// csrc/tools/tt_pid_test.cpp — 底盘直测工具（绕过 Web 层，直接验证 ESP32 链路）
//
// 用法（板上）:
//   ./tt_pid_test [port] [speed] [ms]     默认 /dev/ttyS1, 40, 3000
//
// 流程:
//   打开串口 → 握手(INIT/CONFIG) → set_speed(speed, speed) 保持 ms 毫秒 → brake
//   期间每 200ms 打印 GET_STATUS 轮询的 RPM（确认固件回话）
//
// 判断:
//   1. 打印 "handshake OK" 且轮询 RPM 有值 → 串口/固件链路正常
//   2. 电机不转但 RPM 有值 → 固件 PWM 输出/接线/电源问题
//   3. "tx 0x13 ..." 出现但无 RPM → 固件没回 GET_STATUS（固件版本命令集不同）
//   4. 无任何 tx 日志 → 串口打开失败 / 权限 / 端口号错误

#include "csrc/motor_pair.hpp"
#include "csrc/log.hpp"
#include "csrc/tt_pid.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
    const char* port = argc > 1 ? argv[1] : "/dev/ttyS1";
    int speed = argc > 2 ? std::atoi(argv[2]) : 40;
    int ms = argc > 3 ? std::atoi(argv[3]) : 3000;
    if (speed < -100) speed = -100;
    if (speed > 100) speed = 100;

    CAM_INFO("=== tt_pid test: port=%s speed=%d duration=%dms ===", port, speed, ms);
    CAM_INFO("设置 CSRC_LOG_LEVEL=debug 可看 TX/RX 帧");

    csrc::TtPidChassis chassis(port, 115200, 4680, 20000);
    if (!chassis.ok()) {
        CAM_ERROR("底盘初始化失败: %s", chassis.error().c_str());
        return 1;
    }
    CAM_INFO("handshake OK — 发送速度 %d 保持 %dms", speed, ms);

    chassis.set_speed(speed, speed);
    CAM_INFO("set_speed(%d, %d) 已发送", speed, speed);

    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        int l = 0, r = 0;
        chassis.get_speeds(l, r);
        CAM_INFO("RPM: left=%d right=%d", l, r);
    }

    chassis.brake();
    CAM_INFO("brake 已发送，测试结束");
    return 0;
}
