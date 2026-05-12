#ifndef SMARTCAM_MODULES_INTELLIGENT_ANALYSIS_H
#define SMARTCAM_MODULES_INTELLIGENT_ANALYSIS_H

#include "common/config.h"
#include "common/types.h"
#include "middleware/message_queue.h"
#include <opencv2/opencv.hpp>
#include <memory>
#include <atomic>

namespace smartcam {

class IntelligentAnalysis {
public:
    IntelligentAnalysis(
        std::shared_ptr<MessageQueue<SensorData>> sensor_queue,
        std::shared_ptr<MessageQueue<Frame>> frame_queue);
    ~IntelligentAnalysis();

    bool init(const AnalysisConfig& config, uint32_t base_bitrate_kbps);
    void start();
    void stop();

    uint32_t get_recommended_bitrate() const { return recommended_bitrate_.load(); }
    bool is_motion_detected() const { return motion_detected_.load(); }

    using BitrateCallback = std::function<void(uint32_t)>;
    using OsdCallback = std::function<void(const std::string&)>;
    void set_bitrate_callback(BitrateCallback cb) { bitrate_cb_ = std::move(cb); }
    void set_osd_callback(OsdCallback cb) { osd_cb_ = std::move(cb); }

    bool is_running() const { return running_; }

private:
    void analysis_loop();
    bool detect_motion(const cv::Mat& frame);
    void check_environment_change(const SensorData& data);
    void update_bitrate();
    void update_osd(const SensorData& data);

    std::shared_ptr<MessageQueue<SensorData>> sensor_queue_;
    std::shared_ptr<MessageQueue<Frame>> frame_queue_;

    AnalysisConfig config_;
    uint32_t base_bitrate_kbps_ = 2000;

    cv::Ptr<cv::BackgroundSubtractor> bg_subtractor_;
    cv::Mat prev_frame_;

    std::atomic<bool> running_{false};
    std::atomic<uint32_t> recommended_bitrate_{0};
    std::atomic<bool> motion_detected_{false};

    SensorData last_sensor_data_{};
    SensorData prev_sensor_data_{};
    std::chrono::steady_clock::time_point boost_start_time_;
    bool bitrate_boosted_ = false;

    BitrateCallback bitrate_cb_;
    OsdCallback osd_cb_;
};

} // namespace smartcam

#endif // SMARTCAM_MODULES_INTELLIGENT_ANALYSIS_H
