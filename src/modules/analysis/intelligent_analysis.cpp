#include "intelligent_analysis.h"
#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace smartcam {

IntelligentAnalysis::IntelligentAnalysis(
    std::shared_ptr<MessageQueue<SensorData>> sensor_queue,
    std::shared_ptr<MessageQueue<Frame>> frame_queue)
    : sensor_queue_(std::move(sensor_queue))
    , frame_queue_(std::move(frame_queue)) {}

IntelligentAnalysis::~IntelligentAnalysis() {
    stop();
}

bool IntelligentAnalysis::init(const AnalysisConfig& config, uint32_t base_bitrate_kbps) {
    config_ = config;
    base_bitrate_kbps_ = base_bitrate_kbps;
    recommended_bitrate_ = base_bitrate_kbps;

    if (config.motion_detection.enabled) {
        bg_subtractor_ = cv::createBackgroundSubtractorMOG2(
            500, config.motion_detection.sensitivity * 100, true);
    }

    SPDLOG_INFO("IntelligentAnalysis initialized (motion={}, bitrate_adapt={})",
                config.motion_detection.enabled,
                config.bitrate_adaptation.enabled);
    return true;
}

void IntelligentAnalysis::start() {
    if (running_) return;
    running_ = true;
    std::thread(&IntelligentAnalysis::analysis_loop, this).detach();
    SPDLOG_INFO("IntelligentAnalysis started");
}

void IntelligentAnalysis::stop() {
    running_ = false;
    SPDLOG_INFO("IntelligentAnalysis stopped");
}

void IntelligentAnalysis::analysis_loop() {
    while (running_) {
        SensorData sensor_data;
        while (sensor_queue_->pop(sensor_data, 10)) {
            check_environment_change(sensor_data);
            update_osd(sensor_data);
            prev_sensor_data_ = last_sensor_data_;
            last_sensor_data_ = sensor_data;
        }

        Frame frame;
        if (frame_queue_->pop(frame, 50)) {
            if (config_.motion_detection.enabled) {
            }
        }

        update_bitrate();

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool IntelligentAnalysis::detect_motion(const cv::Mat& frame) {
    if (frame.empty()) return false;

    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, gray, cv::Size(21, 21), 0);

    cv::Mat fg_mask;
    bg_subtractor_->apply(gray, fg_mask);

    cv::threshold(fg_mask, fg_mask, 25, 255, cv::THRESH_BINARY);
    cv::dilate(fg_mask, fg_mask, cv::Mat(), cv::Point(-1, -1), 2);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(fg_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    int total_area = 0;
    for (const auto& contour : contours) {
        int area = static_cast<int>(cv::contourArea(contour));
        if (area > config_.motion_detection.min_area) {
            total_area += area;
        }
    }

    bool detected = total_area > config_.motion_detection.min_area;
    motion_detected_ = detected;

    if (detected) {
        SPDLOG_DEBUG("Motion detected, area={}", total_area);
    }

    return detected;
}

void IntelligentAnalysis::check_environment_change(const SensorData& data) {
    if (!config_.bitrate_adaptation.enabled) return;

    float temp_delta = std::abs(data.temperature - prev_sensor_data_.temperature);
    float humidity_delta = std::abs(data.humidity - prev_sensor_data_.humidity);

    bool significant = (temp_delta > config_.motion_detection.sensitivity * 10.0f) ||
                       (humidity_delta > config_.motion_detection.sensitivity * 20.0f);

    if (significant && !bitrate_boosted_) {
        bitrate_boosted_ = true;
        boost_start_time_ = std::chrono::steady_clock::now();
        uint32_t boosted = static_cast<uint32_t>(
            base_bitrate_kbps_ * config_.bitrate_adaptation.boost_factor);
        recommended_bitrate_ = boosted;
        SPDLOG_INFO("Environment change detected (temp_delta={:.1f}, humidity_delta={:.1f}), "
                    "boosting bitrate to {}kbps",
                    temp_delta, humidity_delta, boosted);

        if (bitrate_cb_) bitrate_cb_(boosted);
    }
}

void IntelligentAnalysis::update_bitrate() {
    if (!bitrate_boosted_) return;

    auto elapsed = std::chrono::steady_clock::now() - boost_start_time_;
    auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

    if (static_cast<uint32_t>(elapsed_sec) >= config_.bitrate_adaptation.recovery_time_sec) {
        bitrate_boosted_ = false;
        recommended_bitrate_ = base_bitrate_kbps_;
        SPDLOG_INFO("Bitrate recovered to {}kbps", base_bitrate_kbps_);

        if (bitrate_cb_) bitrate_cb_(base_bitrate_kbps_);
    }
}

void IntelligentAnalysis::update_osd(const SensorData& data) {
    if (!osd_cb_) return;

    std::ostringstream oss;
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S")
        << "  T:" << std::fixed << std::setprecision(1) << data.temperature << "C"
        << "  H:" << std::setprecision(1) << data.humidity << "%";

    if (motion_detected_) {
        oss << "  [MOTION]";
    }

    osd_cb_(oss.str());
}

} // namespace smartcam
