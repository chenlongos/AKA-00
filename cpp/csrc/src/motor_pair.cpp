// csrc/motor_pair.cpp

#include "csrc/motor_pair.hpp"

#include "csrc/log.hpp"

namespace csrc {

namespace {
/// 包装 TtPidChassis 为 MotorPair（TtPidChassis 本身即满足接口语义）
class TtPidMotorPair : public MotorPair {
public:
    explicit TtPidMotorPair(std::unique_ptr<TtPidChassis> chassis) : chassis_(std::move(chassis)) {}

    void set_speed(int left, int right) override { chassis_->set_speed(left, right); }
    void get_speeds(int& l, int& r) override { chassis_->get_speeds(l, r); }
    void brake() override { chassis_->brake(); }
    void sleep() override { chassis_->sleep(); }
    void close() override { chassis_->close(); }
    bool reinitialize() override { return chassis_->reinitialize(); }
    void get_encoder(int& c1, int& c2) override { chassis_->get_encoder(c1, c2); }
    void move_distance(uint8_t dir, uint8_t speed, int32_t target) override {
        chassis_->move_distance(dir, speed, target);
    }
    void send_cmd_noresp(uint8_t cmd, const uint8_t* payload, size_t len) override {
        chassis_->send_cmd_noresp(cmd, payload, len);
    }

private:
    std::unique_ptr<TtPidChassis> chassis_;
};
}  // namespace

std::unique_ptr<MotorPair> create_motor_pair(const std::string& port,
                                             const std::string& backend,
                                             int baudrate, int ppr) {
    if (backend == "tt_pid") {
        auto chassis = std::make_unique<TtPidChassis>(port, baudrate, ppr, 20000);
        if (!chassis->ok()) {
            CAM_WARN("[motor] tt_pid init failed (%s), falling back to mock", chassis->error().c_str());
            return std::make_unique<MockMotorPair>();
        }
        return std::make_unique<TtPidMotorPair>(std::move(chassis));
    }
    CAM_INFO("[motor] backend=%s → mock", backend.c_str());
    return std::make_unique<MockMotorPair>();
}

}  // namespace csrc
