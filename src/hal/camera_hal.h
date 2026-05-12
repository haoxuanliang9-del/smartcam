#ifndef SMARTCAM_HAL_CAMERA_HAL_H
#define SMARTCAM_HAL_CAMERA_HAL_H

#include <cstdint>
#include <string>
#include <functional>
#include <vector>

namespace smartcam {

struct CameraHalConfig {
    std::string device = "/dev/video0";
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 30;
    std::string format = "YUYV";
};

struct RawFrame {
    uint64_t timestamp;
    std::vector<uint8_t> data;
    uint32_t width;
    uint32_t height;
    std::string format;
};

class CameraHal {
public:
    CameraHal() = default;
    ~CameraHal();

    bool open(const CameraHalConfig& config);
    void close();
    bool is_opened() const { return fd_ >= 0; }

    bool start_capture();
    void stop_capture();

    using FrameCallback = std::function<void(const RawFrame&)>;
    void set_frame_callback(FrameCallback cb);

    bool get_frame(RawFrame& frame, int timeout_ms = 1000);

private:
    int fd_ = -1;
    CameraHalConfig config_;
    FrameCallback callback_;
    std::vector<void*> buffers_;
    bool capturing_ = false;

    bool init_device();
    bool set_format();
    bool request_buffers();
    bool enqueue_buffer();
    bool dequeue_buffer();
};

} // namespace smartcam

#endif // SMARTCAM_HAL_CAMERA_HAL_H
