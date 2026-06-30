// camera_opencv.cpp — macOS OpenCV 摄像头实现
// 编译: 仅 macOS (g++ -std=c++17 `pkg-config --cflags --libs opencv4`)
#ifndef __linux__

#include "camera.h"

#include <opencv2/opencv.hpp>
#include <thread>
#include <chrono>
#include <iostream>

Camera::Camera()  = default;
Camera::~Camera() { close(); }

std::pair<int,int> Camera::target_resolution() {
    int w = 640, h = 480;
    const char* env_w = std::getenv("CAMERA_WIDTH");
    const char* env_h = std::getenv("CAMERA_HEIGHT");
    if (env_w) w = std::atoi(env_w);
    if (env_h) h = std::atoi(env_h);
    if (w <= 0)  w = 640;
    if (h <= 0)  h = 480;
    return {w, h};
}

bool Camera::open(const char* /*unused*/, int target_w, int target_h) {
    auto* cap = new cv::VideoCapture();
    _cap = cap;

    if (!cap->open(0)) {
        std::fprintf(stderr, "[camera] OpenCV: cannot open camera 0\n");
        std::fprintf(stderr, "[camera] OpenCV: check camera permission in System Settings\n");
        delete cap; _cap = nullptr;
        return false;
    }
    cap->set(cv::CAP_PROP_FRAME_WIDTH,  (double)target_w);
    cap->set(cv::CAP_PROP_FRAME_HEIGHT, (double)target_h);
    cap->set(cv::CAP_PROP_FPS, 30.0);

    // 预热：丢弃前几帧
    cv::Mat tmp;
    for (int i = 0; i < 15; i++) *cap >> tmp;

    _width  = (int)cap->get(cv::CAP_PROP_FRAME_WIDTH);
    _height = (int)cap->get(cv::CAP_PROP_FRAME_HEIGHT);
    std::cout << "[camera] OpenCV " << _width << "x" << _height << " ready" << std::endl;
    return true;
}

void Camera::close() {
    if (_cap) {
        auto* cap = static_cast<cv::VideoCapture*>(_cap);
        cap->release();
        delete cap;
        _cap = nullptr;
    }
    std::cout << "[camera] OpenCV closed" << std::endl;
}

bool Camera::good()    const { return _cap != nullptr && static_cast<cv::VideoCapture*>(_cap)->isOpened(); }
int  Camera::fd()      const { return -1; }  // macOS 无 fd
int  Camera::width()   const { return _width; }
int  Camera::height()  const { return _height; }
bool Camera::is_mjpeg() const { return true; }

bool Camera::wait_frame(int timeout_ms) {
    (void)timeout_ms;
    if (!_warmed) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); _warmed = true; }
    return good();
}

std::pair<const uint8_t*, size_t> Camera::read_frame() {
    auto* cap = static_cast<cv::VideoCapture*>(_cap);
    cv::Mat bgr;
    for (int retry = 0; retry < 5; retry++) {
        *cap >> bgr;
        if (!bgr.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (bgr.empty()) return {nullptr, 0};

    std::vector<uchar> jpeg;
    cv::imencode(".jpg", bgr, jpeg, {cv::IMWRITE_JPEG_QUALITY, 70});
    _frame.assign(jpeg.begin(), jpeg.end());
    return {_frame.data(), _frame.size()};
}

#endif  // !__linux__
