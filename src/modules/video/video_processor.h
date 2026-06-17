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

    // Process a YUV420P frame in-place.
    // y_data, u_data, v_data point to the plane buffers.
    // y_stride, uv_stride are the line strides in bytes.
    bool process(uint8_t* y_data, uint8_t* u_data, uint8_t* v_data,
                 int width, int height, int y_stride, int uv_stride);

    bool is_enabled() const { return enabled_; }

    void set_clahe_clip(float limit);
    void set_denoise_strength(float h);

private:
    VideoEnhanceConfig cfg_;
    bool enabled_ = false;
    int frame_count_ = 0;

#ifdef HAS_OPENCV
    cv::Ptr<cv::CLAHE> clahe_;
#endif
};

} // namespace smartcam

#endif // SMARTCAM_MODULES_VIDEO_VIDEO_PROCESSOR_H
