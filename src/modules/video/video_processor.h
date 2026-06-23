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

    // Process Y plane in-place: CLAHE contrast + Bilateral denoise (every frame)
    void process(uint8_t* y_data, int width, int height, int y_stride);

    void set_clahe_clip(float limit);
    void set_denoise_strength(float h);

private:
    VideoEnhanceConfig cfg_;

#ifdef HAS_OPENCV
    cv::Ptr<cv::CLAHE> clahe_;
#endif
};

} // namespace smartcam

#endif // SMARTCAM_MODULES_VIDEO_VIDEO_PROCESSOR_H
