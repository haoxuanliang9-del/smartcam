#include "video_processor.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>

#ifdef HAS_OPENCV
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>
#endif

namespace smartcam {

bool VideoProcessor::init(const VideoEnhanceConfig& cfg) {
    cfg_ = cfg;
    enabled_ = cfg_.enabled;
    frame_count_ = 0;

    if (!enabled_) {
        SPDLOG_INFO("VideoProcessor: disabled, will bypass");
        return true;
    }

#ifdef HAS_OPENCV
    clahe_ = cv::createCLAHE(cfg_.clahe_clip_limit,
                             cv::Size(cfg_.clahe_tile_size, cfg_.clahe_tile_size));
    if (!clahe_) {
        SPDLOG_WARN("VideoProcessor: CLAHE creation failed, continuing with bypass");
        enabled_ = false;
        return false;
    }

    SPDLOG_INFO("VideoProcessor initialized: CLAHE(clip={:.1f}, tile={}), "
                "denoise(h={:.1f}, skip={})",
                cfg_.clahe_clip_limit, cfg_.clahe_tile_size,
                cfg_.denoise_h, cfg_.denoise_skip_frames);
    return true;
#else
    SPDLOG_WARN("VideoProcessor: OpenCV not available, video enhancement disabled");
    enabled_ = false;
    return false;
#endif
}

bool VideoProcessor::process(uint8_t* y_data, uint8_t* u_data, uint8_t* v_data,
                             int width, int height, int y_stride, int uv_stride) {
    if (!enabled_) return true;

#ifdef HAS_OPENCV
    frame_count_++;

    // ── NLMeans denoise (skip-frame) ──
    if (cfg_.denoise_h > 0.0f) {
        int skip = std::max(1, cfg_.denoise_skip_frames + 1); // skip_frames=2 → every 3rd frame, min 1
        if ((frame_count_ % skip) == 0) {
            // Build BGR from YUV for colored denoising
            cv::Mat y_full(height, width, CV_8UC1, y_data, y_stride);
            cv::Mat u_half(height / 2, width / 2, CV_8UC1, u_data, uv_stride);
            cv::Mat v_half(height / 2, width / 2, CV_8UC1, v_data, uv_stride);

            // Upsample UV to full resolution
            cv::Mat u_full, v_full;
            cv::resize(u_half, u_full, cv::Size(width, height), 0, 0, cv::INTER_LINEAR);
            cv::resize(v_half, v_full, cv::Size(width, height), 0, 0, cv::INTER_LINEAR);

            // Merge to BGR
            cv::Mat yuv_channels[3] = {y_full, u_full, v_full};
            cv::Mat yuv, bgr;
            cv::merge(yuv_channels, 3, yuv);
            cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR);

            // NLMeans denoising
            cv::Mat bgr_denoised;
            cv::fastNlMeansDenoisingColored(
                bgr, bgr_denoised,
                static_cast<float>(cfg_.denoise_h),
                static_cast<float>(cfg_.denoise_h),
                7, 21);

            // Convert back to YUV
            cv::Mat yuv_denoised;
            cv::cvtColor(bgr_denoised, yuv_denoised, cv::COLOR_BGR2YUV);
            cv::Mat channels[3];
            cv::split(yuv_denoised, channels);
            cv::Mat& y_denoised = channels[0];
            cv::Mat& u_denoised_full = channels[1];
            cv::Mat& v_denoised_full = channels[2];

            // Write Y back (full res) — row-by-row respecting stride
            for (int r = 0; r < height; r++) {
                std::memcpy(y_data + r * y_stride, y_denoised.ptr<uint8_t>(r), width);
            }

            // Downsample UV back to half-res and write — row-by-row respecting uv_stride
            cv::Mat u_half_out(height / 2, width / 2, CV_8UC1);
            cv::Mat v_half_out(height / 2, width / 2, CV_8UC1);
            cv::resize(u_denoised_full, u_half_out, cv::Size(width / 2, height / 2), 0, 0, cv::INTER_LINEAR);
            cv::resize(v_denoised_full, v_half_out, cv::Size(width / 2, height / 2), 0, 0, cv::INTER_LINEAR);
            for (int r = 0; r < height / 2; r++) {
                std::memcpy(u_data + r * uv_stride, u_half_out.ptr<uint8_t>(r), width / 2);
                std::memcpy(v_data + r * uv_stride, v_half_out.ptr<uint8_t>(r), width / 2);
            }
        }
    }

    // ── CLAHE on Y channel (every frame) — always after denoise ──
    {
        cv::Mat y_channel(height, width, CV_8UC1, y_data, y_stride);
        clahe_->apply(y_channel, y_channel);
    }
#endif

    return true;
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
