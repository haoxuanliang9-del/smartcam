#ifndef SMARTCAM_HAL_V4L2_SOURCE_H
#define SMARTCAM_HAL_V4L2_SOURCE_H

#include "common/types.h"
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <string>

struct SwsContext;

namespace smartcam {

class V4l2Source {
public:
    V4l2Source() = default;
    ~V4l2Source();

    bool open(const std::string& device_path,
              uint32_t requested_width = 1280,
              uint32_t requested_height = 720,
              uint32_t requested_fps = 25,
              const std::string& pixel_format = "YUYV");
    void close();

    bool start();
    void stop();

    bool get_frame(RawFrame& frame, int timeout_ms = 1000);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t fps() const { return fps_; }
    bool is_opened() const { return opened_; }

private:
    void capture_loop();

    std::string device_path_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_ = 25;
    uint32_t v4l2_pixfmt_ = 0;

    int v4l2_fd_ = -1;

    void* mmap_buffers_[4] = {};
    size_t mmap_buffer_sizes_[4] = {};
    int num_buffers_ = 0;

    SwsContext* sws_ctx_ = nullptr;
    int src_pix_fmt_ = 0;
    uint64_t output_frame_count_ = 0;
    uint64_t first_output_ts_us_ = 0;

    std::atomic<bool> opened_{false};
    std::atomic<bool> running_{false};

    std::thread capture_thread_;
    RawFrame latest_frame_;
    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    bool frame_ready_ = false;
};

} // namespace smartcam

#endif // SMARTCAM_HAL_V4L2_SOURCE_H
