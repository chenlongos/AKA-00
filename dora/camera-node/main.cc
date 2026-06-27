/// dora C++ 摄像头节点 —— 使用 dora C API + OpenCV
#include "node_api.h"

#include <opencv2/opencv.hpp>
#include <cstring>
#include <iostream>
#include <string>
#include <chrono>

constexpr int TARGET_WIDTH  = 640;
constexpr int TARGET_HEIGHT = 480;
constexpr int TARGET_FPS    = 30;

int main() {
    std::cout << "[camera-cpp] Starting..." << std::endl;

    // ── 1. 初始化 dora 节点 ──
    void *ctx = init_dora_context_from_env();
    if (!ctx) {
        std::cerr << "[camera-cpp] Failed to init dora context" << std::endl;
        return 1;
    }
    std::cout << "[camera-cpp] Dora context initialized" << std::endl;

    // ── 2. 打开摄像头（始终保持打开，避免 reopen 延迟）──
    cv::VideoCapture cap(0);
    if (!cap.isOpened()) {
        std::cerr << "[camera-cpp] Failed to open camera" << std::endl;
        free_dora_context(ctx);
        return 1;
    }

    cap.set(cv::CAP_PROP_FRAME_WIDTH,  TARGET_WIDTH);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, TARGET_HEIGHT);
    cap.set(cv::CAP_PROP_FPS, TARGET_FPS);

    int actual_w = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
    int actual_h = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    std::cout << "[camera-cpp] Camera opened " << actual_w << "x" << actual_h
              << ", waiting for start command..." << std::endl;

    // ── 3. 事件循环 ──
    cv::Mat frame, rgb;
    bool    capturing   = false;   // 由 control 输入控制
    uint64_t tick_count  = 0;
    auto     last_report = std::chrono::steady_clock::now();
    uint64_t fps_counter = 0;

    while (true) {
        void *event = dora_next_event(ctx);
        if (!event) continue;

        int ev_type = read_dora_event_type((void *)event);

        if (ev_type == DoraEventType_Stop) {
            std::cout << "[camera-cpp] Stop received, exiting" << std::endl;
            free_dora_event(event);
            break;
        }

        if (ev_type == DoraEventType_Input) {
            char *id_ptr = nullptr;
            size_t id_len = 0;
            read_dora_input_id(event, &id_ptr, &id_len);
            std::string id(id_ptr, id_len);

            // ── control 输入：start / stop ──
            if (id == "control") {
                char *data_ptr = nullptr;
                size_t data_len = 0;
                read_dora_input_data(event, &data_ptr, &data_len);
                std::string cmd(data_ptr, data_len);

                if (cmd == "start") {
                    capturing = true;
                    std::cout << "[camera-cpp] ▶  capture started" << std::endl;
                } else if (cmd == "stop") {
                    capturing = false;
                    std::cout << "[camera-cpp] ⏸  capture stopped" << std::endl;
                }
                free_dora_event(event);
                continue;
            }

            // ── tick 输入：只在 capturing 时抓帧 ──
            if (id == "tick" && capturing) {
                auto t0 = std::chrono::steady_clock::now();

                cap >> frame;
                if (frame.empty()) {
                    free_dora_event(event);
                    continue;
                }

                // BGR → RGB
                cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);

                int data_len = rgb.total() * rgb.elemSize();

                dora_send_output(
                    ctx,
                    (char *)"image", 5,
                    (char *)rgb.data, data_len
                );

                tick_count++;
                fps_counter++;

                auto capture_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();

                // 每秒统计
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_report).count();
                if (elapsed >= 1000) {
                    double fps = fps_counter * 1000.0 / elapsed;
                    std::cout << "[camera-cpp] " << rgb.cols << "x" << rgb.rows
                              << " raw RGB | capture: " << capture_ms << "ms | "
                              << (int)fps << " fps" << std::endl;
                    fps_counter = 0;
                    last_report = std::chrono::steady_clock::now();
                }
            }
        }

        free_dora_event(event);
    }

    // ── 4. 清理 ──
    cap.release();
    free_dora_context(ctx);
    std::cout << "[camera-cpp] Shutting down (" << tick_count << " frames)" << std::endl;
    return 0;
}
