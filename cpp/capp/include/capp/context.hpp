// capp/context.hpp — 应用共享状态（对应 app/services/* 的全局单例）
//
// AppContext 持有全部服务状态：
//   - 硬件: MotorPair / Gripper / Camera（csrc）
//   - 状态采集: StateCollector（csrc 单例）
//   - 控制服务: 定时停线程 / 夹爪锁
//   - demo: 运行进程 + 下载进度
//   - ota: 升级任务
//   - 云端上报: 命令日志
//
// 服务方法实现见 services.cpp；路由处理见 routes.cpp。

#pragma once

#include <atomic>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "csrc/angle_config.hpp"
#include "csrc/camera.hpp"
#include "csrc/config.hpp"
#include "csrc/gripper.hpp"
#include "csrc/json.hpp"
#include "csrc/motor_pair.hpp"
#include "csrc/state.hpp"

namespace capp {

struct AppContext {
    csrc::Config config;
    std::unique_ptr<csrc::MotorPair> motor_pair;
    std::unique_ptr<csrc::Gripper> gripper;
    csrc::StateCollector& collector = csrc::StateCollector::get_instance();
    csrc::Camera& camera = csrc::Camera::get_instance();

    bool camera_on = false;

    /// 优雅关闭标记（SIGTERM/SIGINT → main 设置 → accept 循环退出）
    std::atomic<bool> shutdown{false};

    // 控制服务
    std::mutex timer_mu;
    std::thread* timer_thread = nullptr;
    std::atomic<bool> timer_cancel{false};
    std::mutex arm_mu;   // grab/release 串行

    // demo 状态
    std::mutex demo_mu;
    pid_t demo_pid = -1;
    int demo_pgid = -1;
    std::string demo_name;

    // demo 模型下载进度: task_id → Json{progress, status, error}
    std::mutex dl_mu;
    std::map<std::string, csrc::Json> downloads;

    // ota 任务: task_id → Json{progress, status, message}
    std::mutex ota_mu;
    std::map<std::string, csrc::Json> ota_tasks;

    // 云端上报命令日志
    std::mutex cmdlog_mu;
    std::vector<csrc::Json> command_log;

    // 路径
    std::string app_dir;      // 项目根（含 static/、demo/、arm_angles.json）
    std::string static_dir;   // 前端静态目录
    std::string version;      // VERSION 文件内容（OTA /version 用）

    /// 记录控制命令（status_reporter 上报用）
    void log_command(csrc::Json cmd) {
        std::lock_guard<std::mutex> lk(cmdlog_mu);
        csrc::Json entry;
        entry["ts"] = csrc::Json((int64_t)(::time(nullptr)));
        for (auto& kv : cmd.object()) entry[kv.first] = kv.second;
        command_log.push_back(entry);
        if (command_log.size() > 20) command_log.erase(command_log.begin());
    }
};

// ── 控制服务（对应 app/services/control_service.py）──

/// 初始化硬件服务（启动 StateCollector）。返回 false 表示全部 mock。
bool init_services(AppContext& ctx);

/// 动作控制: action = up/down/left/right/stop/grab/release
csrc::Json execute_action(AppContext& ctx, const std::string& action, int speed, double milliseconds);
/// 直接设置电机速度（可选持续时间秒）
csrc::Json run_motor(AppContext& ctx, int left, int right, double duration);
/// 闭环距离/转向（ESP32 固件内部执行）
csrc::Json move_distance(AppContext& ctx, const std::string& direction, double value, int speed);
/// 发送原始命令到夹爪串口
csrc::Json send_raw_command(AppContext& ctx, const std::string& cmd);
/// 更新机械臂角度配置
csrc::Json update_arm_angles(AppContext& ctx, const std::string& driver, const csrc::Json& angles);
/// 预览机械臂角度（立即执行）
csrc::Json preview_arm_angle(AppContext& ctx, const std::string& driver, const std::string& key, int angle);
/// 重新初始化底盘
csrc::Json reinitialize_motor_pair(AppContext& ctx);

// ── 摄像头服务（对应 app/services/camera_service.py）──

/// 确保摄像头已打开
bool ensure_camera(AppContext& ctx);
/// 关闭摄像头
void close_camera(AppContext& ctx);
/// 当前帧 → JPEG 字节（原生 MJPEG 直通；YUYV 先转 RGB 再编码）
bool current_jpeg(AppContext& ctx, int quality, std::vector<uint8_t>& out);

// ── 状态上报（对应 app/services/status_reporter.py）──

/// 启动云端状态上报线程（URL 为空则不启动）
void start_status_reporter(AppContext& ctx);
/// 立即上报一次（boot/heartbeat）
void report_status(AppContext& ctx, const std::string& action);

}  // namespace capp
