#ifndef SMARTCAM_MODULES_CAMERA_CAPTURE_H
#define SMARTCAM_MODULES_CAMERA_CAPTURE_H

#include "common/config.h"
#include "common/types.h"
#include "hal/video_file_source.h"
#include "middleware/message_queue.h"
#include <functional>
#include <memory>
#include <atomic>
#include <fstream>
#include <vector>
#include <mutex>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVFilterGraph;
struct AVFilterContext;
struct AVBSFContext;

namespace smartcam {

class CameraCapture {
public:
    CameraCapture() = default;
    ~CameraCapture();

    bool init(const CameraConfig& config, const OsdConfig& osd_config);
    void start();
    void stop();

    void set_bitrate(uint32_t bitrate_kbps);
    void update_osd_text(const std::string& text);

    void add_client_queue(std::shared_ptr<MessageQueue<std::shared_ptr<Frame>>> queue);
    void remove_client_queue(std::shared_ptr<MessageQueue<std::shared_ptr<Frame>>> queue);

    using BitrateCallback = std::function<void(uint32_t)>;
    void set_actual_bitrate_callback(BitrateCallback cb) { actual_bitrate_cb_ = std::move(cb); }

    bool is_running() const { return running_; }

private:
    void capture_loop();
    bool init_encoder();
    bool init_osd_filter();
    bool encode_frame(AVFrame* frame, uint64_t t_decode, uint32_t frame_seq);

    std::vector<std::shared_ptr<MessageQueue<std::shared_ptr<Frame>>>> client_queues_;
    std::mutex queues_mutex_;
    VideoFileSource video_source_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_ = 25;
    uint32_t bitrate_kbps_ = 1000;
    OsdConfig osd_config_;

    AVCodecContext* encoder_ctx_ = nullptr;
    AVFrame* yuv_frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* filter_frame_ = nullptr;

    AVFilterGraph* filter_graph_ = nullptr;
    AVFilterContext* buffersrc_ctx_ = nullptr;
    AVFilterContext* buffersink_ctx_ = nullptr;
    AVFilterContext* drawtext_ctx_ = nullptr;

    std::string osd_textfile_path_;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> target_bitrate_{0};

    BitrateCallback actual_bitrate_cb_;
    std::atomic<uint64_t> bytes_encoded_{0};
    std::chrono::steady_clock::time_point last_bitrate_time_;
    std::atomic<uint32_t> actual_bitrate_kbps_{0};

    AVBSFContext* annexb_bsf_ = nullptr;
};

} // namespace smartcam

#endif // SMARTCAM_MODULES_CAMERA_CAPTURE_H
