#ifndef SMARTCAM_HAL_VIDEO_FILE_SOURCE_H
#define SMARTCAM_HAL_VIDEO_FILE_SOURCE_H

#include "common/types.h"
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <string>

struct AVFormatContext;
struct AVCodecContext;
struct SwsContext;

namespace smartcam {

class VideoFileSource {
public:
    VideoFileSource() = default;
    ~VideoFileSource();

    bool open(const std::string& file_path);
    void close();

    bool start();
    void stop();

    bool get_frame(RawFrame& frame, int timeout_ms = 1000);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t fps() const { return fps_; }
    bool is_opened() const { return opened_; }

private:
    void read_loop();
    bool seek_to_beginning();

    std::string file_path_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_ = 25;

    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* decode_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    int video_stream_idx_ = -1;

    std::atomic<bool> opened_{false};
    std::atomic<bool> running_{false};

    std::thread read_thread_;
    RawFrame latest_frame_;
    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    bool frame_ready_ = false;
};

} // namespace smartcam

#endif // SMARTCAM_HAL_VIDEO_FILE_SOURCE_H
