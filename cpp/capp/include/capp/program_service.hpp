#pragma once
#include <memory>
#include "csrc/json.hpp"

namespace capp {
struct AppContext;
class ProgramService {
public:
    explicit ProgramService(AppContext& ctx);
    ~ProgramService();
    csrc::Json capabilities() const;
    csrc::Json upload(const csrc::Json& package, int& http_status);
    csrc::Json deploy(const csrc::Json& package, int& http_status);
    csrc::Json run(const std::string& program_id, int& http_status);
    csrc::Json stop(const std::string& run_id, int& http_status);
    csrc::Json status() const;
    csrc::Json logs(const std::string& run_id, int64_t after) const;
    // Call once after listening and before accepting controls. /tmp must be tmpfs.
    void start_autorun(const std::string& boot_marker = "/tmp/aka-blockly-autorun.claim");
    void shutdown();
private:
    csrc::Json save_package(const csrc::Json& package, int& http_status, bool autorun);
    csrc::Json run_locked(const std::string& program_id, int& http_status, bool automatic,
                         int64_t expected_motion_seq = -1);
    csrc::Json run_internal(const std::string& program_id, int& http_status, bool automatic);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace capp
