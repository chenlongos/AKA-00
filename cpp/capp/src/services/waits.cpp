// 阻塞等待原语
//
// 由 capp/src/services.cpp 按域拆出来（对应 app/services/*.py 的分法）。
// 给 HTTP 层用：等机械臂动作做完、等脚本跑完（每个连接一个线程，阻塞不挡别的请求）。
// 声明都在 capp/context.hpp（那一份是按域分节的伞头文件，调用方只 include 它）。

#include "capp/context.hpp"

#include <chrono>
#include <thread>

namespace capp {

void wait_arm_done(AppContext& ctx) {
    // grab/release 是丢给后台线程跑的（ZP10S 那套"伸下去→夹→抬起"约 3.5s），
    // 线程全程持 arm_mu —— 所以这里拿得到锁就说明上一段动作已经结束。
    // 有界：那段序列是固定时长 + 串口自带超时，不会无限等。
    std::lock_guard<std::mutex> lk(ctx.arm_mu);
}

bool wait_script_done(AppContext& ctx, double timeout_s) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds((long long)(timeout_s * 1000.0));
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lk(ctx.script_mu);
            if (!ctx.script_running) return true;   // 跑完了（正常/失败/被停都算）
        }
        if (ctx.shutdown) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;   // 超时还在跑
}

}  // namespace capp
