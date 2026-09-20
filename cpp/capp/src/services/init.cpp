// 服务层装配
//
// 由 capp/src/services.cpp 按域拆出来（对应 app/services/*.py 的分法）。
// main.cpp 启动时调一次：配置、底盘、夹爪、状态采集。
// 声明都在 capp/context.hpp（那一份是按域分节的伞头文件，调用方只 include 它）。

#include "capp/context.hpp"

#include "csrc/config.hpp"
#include "csrc/gripper.hpp"
#include "csrc/log.hpp"
#include "csrc/motor_pair.hpp"

namespace capp {

bool init_services(AppContext& ctx) {
    ctx.config = csrc::Config::load();

    // 底盘（先建：tt_pid 为自动重连代理，构造不阻塞、失败自动后台重试）
    ctx.motor_pair = csrc::create_motor_pair(ctx.config.motor.port, ctx.config.motor.backend,
                                             ctx.config.motor.baudrate, ctx.config.motor.ppr);
    ctx.motor_link = dynamic_cast<csrc::AutoReconnectMotorPair*>(ctx.motor_pair.get());
    ctx.collector.set_motor_pair(ctx.motor_pair.get());

    // 夹爪
    ctx.gripper = csrc::create_gripper(ctx.config.arm.backend, ctx.config.arm.port,
                                       ctx.config.arm.baudrate);

    // 状态采集
    ctx.collector.set_wheel_diameter_mm(ctx.config.chassis.wheel_diameter_mm);
    ctx.collector.set_gripper_status_provider([&ctx] {
        return std::string(csrc::gripper_status_str(ctx.gripper->get_status()));
    });
    ctx.collector.start();

    // 底盘和夹爪都没有 mock 了（2026-09-18 删除）：接不上就是明确报错 + 状态里体现，
    // 不再有"假装能动"的路径。返回值（"是否全是 mock"）恒为 true（没有 mock 可退）。
    CAM_INFO("[app] services ready (motor=%s arm=%s)",
             ctx.config.motor.backend.c_str(), ctx.config.arm.backend.c_str());
    return true;
}

}  // namespace capp
