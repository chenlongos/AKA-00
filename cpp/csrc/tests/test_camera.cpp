// Exercise the real Camera and JPEG code without a camera or robot attached.
// Only kernel device operations are simulated.
#include "csrc/camera.hpp"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace {
enum class Device { Missing, Cvi, V4l2, PartialV4l2 };
Device device = Device::Missing;
std::vector<uint8_t> jpeg;
std::atomic<int> fds{0}, maps{0}, stream_offs{0};
constexpr int camera_fd = 123456;
struct CameraInfo { uint16_t width, height; uint8_t format, connected; };
void* first_buffer = nullptr;

void reset(Device next) {
    assert(fds == 0 && maps == 0);
    device = next;
    stream_offs = 0;
    first_buffer = nullptr;
}

void frame_arrives(csrc::Camera& camera) {
    csrc::Camera::Frame frame;
    for (int i = 0; i < 1000; ++i) {
        if (camera.read_latest(frame) && !frame.data.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(frame.data == jpeg && frame.w == 16 && frame.h == 12);
    csrc::Camera::RgbFrame rgb;
    assert(camera.latest_rgb(0, rgb));
    assert(rgb.w == 16 && rgb.h == 12 && rgb.data.size() == 16 * 12 * 3);
}
}  // namespace

extern "C" {
int __real_open(const char*, int, ...);
int __real_close(int);
int __real_access(const char*, int);
void* __real_mmap(void*, size_t, int, int, int, off_t);
int __real_munmap(void*, size_t);
int __real_poll(pollfd*, nfds_t, int);

int __wrap_access(const char* path, int mode) {
    if (std::strncmp(path, "/dev/", 5) != 0) return __real_access(path, mode);
    const bool cvi = std::strstr(path, "cvi-usb-camera") != nullptr;
    if ((device == Device::Cvi && cvi) ||
        ((device == Device::V4l2 || device == Device::PartialV4l2) && !cvi)) return 0;
    errno = ENOENT;
    return -1;
}

int __wrap_open(const char* path, int flags, ...) {
    if (std::strncmp(path, "/dev/", 5) == 0) {
        if (__wrap_access(path, F_OK) != 0) return -1;
        assert(flags & O_CLOEXEC);
        ++fds;
        return camera_fd;
    }
    va_list args;
    va_start(args, flags);
    int mode = (flags & O_CREAT) ? va_arg(args, int) : 0;
    va_end(args);
    return __real_open(path, flags, mode);
}

int __wrap_close(int fd) {
    if (fd != camera_fd) return __real_close(fd);
    --fds;
    return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...) {
    assert(fd == camera_fd);
    if (device == Device::Cvi && request == 1) return 0;
    va_list args;
    va_start(args, request);
    void* arg = va_arg(args, void*);
    va_end(args);
    if (device == Device::Cvi) {
        if (request == 2) {
            *static_cast<CameraInfo*>(arg) = {16, 12, 1, 1};
            return 0;
        }
        assert(request == 3); // CVI must not receive V4L2 STREAMOFF.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::memcpy(arg, jpeg.data(), jpeg.size());
        return static_cast<int>(jpeg.size());
    }
    switch (request) {
    case VIDIOC_QUERYCAP:
        static_cast<v4l2_capability*>(arg)->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
        return 0;
    case VIDIOC_S_FMT:
    case VIDIOC_QBUF:
    case VIDIOC_STREAMON:
        return 0;
    case VIDIOC_REQBUFS:
        static_cast<v4l2_requestbuffers*>(arg)->count = 2;
        return 0;
    case VIDIOC_QUERYBUF: {
        auto* buffer = static_cast<v4l2_buffer*>(arg);
        if (device == Device::PartialV4l2 && buffer->index == 1) return -1;
        buffer->length = jpeg.size();
        buffer->m.offset = buffer->index * 4096;
        return 0;
    }
    case VIDIOC_DQBUF: {
        auto* buffer = static_cast<v4l2_buffer*>(arg);
        buffer->index = 0;
        buffer->bytesused = jpeg.size();
        return 0;
    }
    case VIDIOC_STREAMOFF:
        ++stream_offs;
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

void* __wrap_mmap(void* ptr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (fd != camera_fd) return __real_mmap(ptr, length, prot, flags, fd, offset);
    void* buffer = std::malloc(length);
    assert(buffer);
    std::memcpy(buffer, jpeg.data(), length);
    if (offset == 0) first_buffer = buffer;
    ++maps;
    return buffer;
}

int __wrap_munmap(void* ptr, size_t) {
    std::free(ptr);
    --maps;
    return 0;
}

int __wrap_poll(pollfd* pfd, nfds_t count, int timeout) {
    if (count != 1 || pfd->fd != camera_fd) return __real_poll(pfd, count, timeout);
    assert(first_buffer);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    pfd->revents = POLLIN;
    return 1;
}
}  // extern "C"

int main() {
    std::vector<uint8_t> pixels(16 * 12 * 3, 127), decoded;
    assert(csrc::Camera::rgb_to_jpeg(pixels.data(), 16, 12, 85, jpeg));
    int w = 0, h = 0;
    assert(csrc::Camera::jpeg_to_rgb(jpeg.data(), jpeg.size(), w, h, decoded));
    assert(w == 16 && h == 12 && decoded.size() == pixels.size());
    for (Device native : {Device::Cvi, Device::V4l2}) {
        reset(native);
        csrc::Camera camera;
        // /dev/video0 automatically falls back to CVI when the Linux node is absent.
        assert(camera.open("/dev/video0", 16, 12, 15));
        frame_arrives(camera);
        assert(camera.open("/dev/video0", 16, 12, 15));
        assert(fds == 1);
        camera.close();
        assert(!camera.is_available() && fds == 0 && maps == 0);
        csrc::Camera::Frame stale;
        csrc::Camera::RgbFrame rgb;
        assert(!camera.read_latest(stale) && !camera.latest_rgb(0, rgb));
        assert(stream_offs == (native == Device::Cvi ? 0 : 1));
        camera.close();
        assert(camera.open("/dev/video0", 16, 12, 15));
        frame_arrives(camera);
        camera.close();
    }
    for (Device absent : {Device::Missing, Device::PartialV4l2}) {
        reset(absent);
        csrc::Camera camera;
        assert(!camera.open("/dev/video0", 16, 12, 15));
        assert(fds == 0 && maps == 0);
        camera.close();
        assert(!camera.is_available());
    }
    std::puts("Camera: CVI, V4L2, reopen, stale frames and partial-open cleanup: PASSED");
}
