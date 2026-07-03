/// dora C++ 摄像头节点
///   板子 (Linux):   V4L2 直接采集 MJPEG
///   开发 (macOS):   OpenCV VideoCapture → JPEG
///
/// 日志由 [logging] level（config.toml）控制，由 init.sh 透传成 CAMERA_LOG_LEVEL
/// 环境变量；默认 info（不打印每秒的 fps 报告）。要看实时帧率就改 level=debug。

#include "node_api.h"
#include "camera.h"
#include "log.h"

#include <string>
#include <chrono>

int main() {
    CAM_INFO("Starting (log level: %s)",
             camlog::level_name(camlog::current_level()));

    void* ctx = init_dora_context_from_env();
    if (!ctx) {
        CAM_ERROR("Failed to init dora context");
        return 1;
    }
    CAM_INFO("Dora context initialized");

    Camera   cam;
    bool     capturing   = false;
    uint64_t tick_count  = 0;
    auto     last_report = std::chrono::steady_clock::now();
    uint64_t fps_counter = 0;

    // 从环境变量读取目标分辨率
    auto [target_w, target_h] = Camera::target_resolution();
    CAM_INFO("target: %dx%d", target_w, target_h);

    while (true) {
        void* event = dora_next_event(ctx);
        if (!event) continue;

        int ev_type = read_dora_event_type(event);

        if (ev_type == DoraEventType_Stop) {
            CAM_INFO("Stop, exiting");
            free_dora_event(event);
            break;
        }

        if (ev_type == DoraEventType_Input) {
            char* id_ptr = nullptr;
            size_t id_len = 0;
            read_dora_input_id(event, &id_ptr, &id_len);
            std::string id(id_ptr, id_len);

            if (id == "control") {
                char* data_ptr = nullptr;
                size_t data_len = 0;
                read_dora_input_data(event, &data_ptr, &data_len);
                std::string cmd(data_ptr, data_len);

                if (cmd == "start") {
                    if (!cam.good()) {
                        // 首次打开设备
                        if (!cam.open(nullptr, target_w, target_h)) {
                            CAM_ERROR("Failed to open camera");
                            free_dora_event(event);
                            continue;
                        }
                    } else {
                        // 设备已打开，只需恢复流（避免 reopen 开销）
                        cam.start_stream();
                    }
                    capturing = true;
                    CAM_INFO("▶  capture started");
                } else if (cmd == "stop") {
                    capturing = false;
                    cam.stop_stream();  // 只停流，保留 fd 和 buffer
                    CAM_INFO("⏸  capture stopped");
                }
                free_dora_event(event);
                continue;
            }

            if (id == "tick" && cam.good() && capturing) {
                free_dora_event(event);

                if (!cam.wait_frame(2000)) continue;

                auto [data, len] = cam.read_frame();
                if (!data || len == 0) continue;

                int rc = dora_send_output(ctx, (char*)"image", 5, (char*)data, (size_t)len);
                if (rc != 0) {
                    static int send_errs = 0;
                    if (send_errs++ < 3 || send_errs % 100 == 0)
                        CAM_WARN("dora_send_output FAILED (rc=%d, err#%d)", rc, send_errs);
                    continue;
                }

                tick_count++;
                fps_counter++;

                // 1Hz 帧率报告——只在 debug / trace 级别打印，默认 info 不刷屏
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_report).count();
                if (elapsed >= 1000) {
                    double fps = fps_counter * 1000.0 / elapsed;
                    CAM_DEBUG("%dx%d %s | %d fps | %llu KB/frame",
                              cam.width(), cam.height(),
                              cam.is_mjpeg() ? "MJPEG" : "YUYV",
                              (int)fps,
                              (unsigned long long)(len / 1024));
                    fps_counter = 0;
                    last_report = std::chrono::steady_clock::now();
                }
                continue;
            }
        }
        free_dora_event(event);
    }

    if (cam.good()) cam.close();
    free_dora_context(ctx);
    CAM_INFO("Shutdown (%llu frames)", (unsigned long long)tick_count);
    return 0;
}
