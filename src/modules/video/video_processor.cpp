#include "video_processor.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>

#ifdef HAS_OPENCV
#include <opencv2/imgproc.hpp>
#endif

namespace smartcam {

bool VideoProcessor::init(const VideoEnhanceConfig& cfg) {
    cfg_ = cfg;
    frame_count_ = 0;

#ifdef HAS_OPENCV
    clahe_ = cv::createCLAHE(cfg_.clahe_clip_limit,
                             cv::Size(cfg_.clahe_tile_size, cfg_.clahe_tile_size));
    if (!clahe_) {
        SPDLOG_WARN("VideoProcessor: CLAHE creation failed");
        return false;
    }

    SPDLOG_INFO("VideoProcessor initialized: CLAHE(clip={:.1f}, tile={}), "
                "denoise(h={:.1f}, skip={})",
                cfg_.clahe_clip_limit, cfg_.clahe_tile_size,
                cfg_.denoise_h, cfg_.denoise_skip_frames);
    return true;
#else
    SPDLOG_ERROR("VideoProcessor: OpenCV not available, video enhancement unavailable");
    return false;
#endif
}

void VideoProcessor::apply_clahe(uint8_t* y_data, int width, int height, int y_stride) {
#ifdef HAS_OPENCV
    cv::Mat y_channel(height, width, CV_8UC1, y_data, y_stride);
    clahe_->apply(y_channel, y_channel);
#endif
}

void VideoProcessor::apply_denoise(uint8_t* y_data, int width, int height, int y_stride) {
#ifdef HAS_OPENCV
    if (cfg_.denoise_h <= 0.0f) return;

    cv::Mat y_channel(height, width, CV_8UC1, y_data, y_stride);
    cv::Mat y_filtered;
    int d = std::max(3, std::min(15, static_cast<int>(cfg_.denoise_h)));
    double sigma_c = static_cast<double>(cfg_.denoise_h) * 2.0;
    double sigma_s = static_cast<double>(cfg_.denoise_h) * 0.5;
    cv::bilateralFilter(y_channel, y_filtered, d, sigma_c, sigma_s);
    for (int r = 0; r < height; r++) {
        std::memcpy(y_data + r * y_stride, y_filtered.ptr<uint8_t>(r), width);
    }
#endif
}

void VideoProcessor::set_clahe_clip(float limit) {
    cfg_.clahe_clip_limit = std::max(0.1f, limit);
#ifdef HAS_OPENCV
    if (clahe_) {
        clahe_->setClipLimit(cfg_.clahe_clip_limit);
    }
#endif
    SPDLOG_DEBUG("VideoProcessor: clahe_clip_limit = {:.1f}", cfg_.clahe_clip_limit);
}

void VideoProcessor::set_denoise_strength(float h) {
    cfg_.denoise_h = std::max(0.0f, h);
    SPDLOG_DEBUG("VideoProcessor: denoise_h = {:.1f}", cfg_.denoise_h);
}

} // namespace smartcam
