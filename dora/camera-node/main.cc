/// dora C++ 摄像头节点
///   板子 (Linux):   V4L2 直接采集 MJPEG
///   开发 (macOS):   OpenCV VideoCapture → JPEG

#include "node_api.h"
#include "camera.h"

#include <iostream>
#include <string>
#include <chrono>

int main() {
    std::cout << "[camera-cpp] Starting..." << std::endl;

    void* ctx = init_dora_context_from_env();
    if (!ctx) {
        std::cerr << "[camera-cpp] Failed to init dora context" << std::endl;
        return 1;
    }
    std::cout << "[camera-cpp] Dora context initialized" << std::endl;

    Camera   cam;
    bool     capturing   = false;
    uint64_t tick_count  = 0;
    auto     last_report = std::chrono::steady_clock::now();
    uint64_t fps_counter = 0;

    while (true) {
        void* event = dora_next_event(ctx);
        if (!event) continue;

        int ev_type = read_dora_event_type(event);

        if (ev_type == DoraEventType_Stop) {
            std::cout << "[camera-cpp] Stop, exiting" << std::endl;
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
                    if (!cam.good() && !cam.open()) {
                        std::cerr << "[camera-cpp] Failed to open camera" << std::endl;
                        free_dora_event(event);
                        continue;
                    }
                    capturing = true;
                    std::cout << "[camera-cpp] ▶  capture started" << std::endl;
                } else if (cmd == "stop") {
                    capturing = false;
                    if (cam.good()) cam.close();
                    std::cout << "[camera-cpp] ⏸  capture stopped" << std::endl;
                }
                free_dora_event(event);
                continue;
            }

            if (id == "tick" && cam.good() && capturing) {
                free_dora_event(event);

                if (!cam.wait_frame(2000)) continue;

                auto [data, len] = cam.read_frame();
                if (!data || len == 0) continue;

                dora_send_output(ctx, (char*)"image", 5, (char*)data, (size_t)len);

                tick_count++;
                fps_counter++;

                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_report).count();
                if (elapsed >= 1000) {
                    double fps = fps_counter * 1000.0 / elapsed;
                    std::cout << "[camera-cpp] " << cam.width() << "x" << cam.height()
                              << " " << (cam.is_mjpeg() ? "MJPEG" : "YUYV")
                              << " | " << (int)fps << " fps | "
                              << len / 1024 << " KB/frame" << std::endl;
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
    std::cout << "[camera-cpp] Shutdown (" << tick_count << " frames)" << std::endl;
    return 0;
}
