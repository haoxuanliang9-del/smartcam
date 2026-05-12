#ifndef SMARTCAM_MODULES_CAMERA_CAPTURE_H
#define SMARTCAM_MODULES_CAMERA_CAPTURE_H

#include "common/config.h"
#include "common/types.h"
#include "hal/camera_hal.h"
#include "middleware/message_queue.h"
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVFilterGraph;
struct AVFilterContext;
struct AVBufferRef;

namespace smartcam {

class CameraCapture {
public:
    explicit CameraCapture(std::shared_ptr<MessageQueue<Frame>> output_queue);
    ~CameraCapture();

    bool init(const CameraConfig& config, const OsdConfig& osd_config);
    void start();
    void stop();

    void set_bitrate(uint32_t bitrate_kbps);
    void update_osd_text(const std::string& text);

    bool is_running() const { return running_; }

private:
    void capture_loop();
    bool init_encoder();
    bool init_osd_filter();
    bool encode_frame(AVFrame* frame);

    std::shared_ptr<MessageQueue<Frame>> output_queue_;
    CameraHal camera_hal_;
    CameraConfig camera_config_;
    OsdConfig osd_config_;

    AVCodecContext* encoder_ctx_ = nullptr;
    AVFrame* yuv_frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* filter_frame_ = nullptr;

    AVFilterGraph* filter_graph_ = nullptr;
    AVFilterContext* buffersrc_ctx_ = nullptr;
    AVFilterContext* buffersink_ctx_ = nullptr;
    AVFilterContext* drawtext_ctx_ = nullptr;

    std::string osd_text_;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> target_bitrate_{0};
    std::atomic<bool> osd_update_pending_{false};
    std::mutex osd_mutex_;
};

} // namespace smartcam

#endif // SMARTCAM_MODULES_CAMERA_CAPTURE_H
