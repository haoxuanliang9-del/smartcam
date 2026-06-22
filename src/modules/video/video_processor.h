#ifndef SMARTCAM_MODULES_VIDEO_VIDEO_PROCESSOR_H
#define SMARTCAM_MODULES_VIDEO_VIDEO_PROCESSOR_H

#include "common/config.h"
#include <cstdint>

#ifdef HAS_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#endif

namespace smartcam {

class VideoProcessor {
public:
    VideoProcessor() = default;
    ~VideoProcessor() = default;

    VideoProcessor(const VideoProcessor&) = delete;
    VideoProcessor& operator=(const VideoProcessor&) = delete;

    bool init(const VideoEnhanceConfig& cfg);

    // Apply CLAHE on Y plane (fast, ~2ms, every frame). Caller: capture thread.
    void apply_clahe(uint8_t* y_data, int width, int height, int y_stride);

    // Apply NLMeans denoise on Y plane (slow, ~240ms, skip-frame). Caller: process thread.
    void apply_denoise(uint8_t* y_data, int width, int height, int y_stride);

    void set_clahe_clip(float limit);
    void set_denoise_strength(float h);

private:
    VideoEnhanceConfig cfg_;
    int frame_count_ = 0;

#ifdef HAS_OPENCV
    cv::Ptr<cv::CLAHE> clahe_;
#endif
};

} // namespace smartcam

#endif // SMARTCAM_MODULES_VIDEO_VIDEO_PROCESSOR_H
