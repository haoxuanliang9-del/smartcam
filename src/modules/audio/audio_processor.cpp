#include "audio_processor.h"
#include <spdlog/spdlog.h>
#include <rnnoise.h>
#include <cmath>
#include <algorithm>
#include <cstring>

extern "C" {
#include <libswresample/swresample.h>
}

namespace smartcam {

AudioProcessor::~AudioProcessor() {
    if (rnnoise_state_) {
        rnnoise_destroy(rnnoise_state_);
        rnnoise_state_ = nullptr;
    }
    if (swr_) {
        swr_free(&swr_);
        swr_ = nullptr;
    }
}

bool AudioProcessor::init(const AudioEnhanceConfig& cfg) {
    cfg_ = cfg;

    // Init RNNoise
    rnnoise_state_ = rnnoise_create(nullptr); // nullptr = use built-in model
    if (!rnnoise_state_) {
        SPDLOG_ERROR("AudioProcessor: rnnoise_create failed");
        return false;
    }

    if (rnnoise_get_frame_size() != 480) {
        SPDLOG_ERROR("AudioProcessor: unexpected RNNoise frame size {}",
                     rnnoise_get_frame_size());
        rnnoise_destroy(rnnoise_state_);
        rnnoise_state_ = nullptr;
        return false;
    }

    // Init AGC smoothing coefficients
    constexpr float frame_ms = 10.0f;
    agc_attack_coeff_ = std::exp(-frame_ms / cfg_.agc_attack_ms);
    agc_release_coeff_ = std::exp(-frame_ms / cfg_.agc_release_ms);
    agc_current_gain_ = 1.0f;

    // Init swresample: 48kHz int16 mono → 8kHz int16 mono
    swr_ = swr_alloc_set_opts(nullptr,
        AV_CH_LAYOUT_MONO, AV_SAMPLE_FMT_S16, 8000,
        AV_CH_LAYOUT_MONO, AV_SAMPLE_FMT_S16, 48000,
        0, nullptr);
    if (!swr_) {
        SPDLOG_ERROR("AudioProcessor: swr_alloc_set_opts failed");
        rnnoise_destroy(rnnoise_state_);
        rnnoise_state_ = nullptr;
        return false;
    }
    if (swr_init(swr_) < 0) {
        SPDLOG_ERROR("AudioProcessor: swr_init failed");
        swr_free(&swr_);
        rnnoise_destroy(rnnoise_state_);
        rnnoise_state_ = nullptr;
        return false;
    }

    SPDLOG_INFO("AudioProcessor initialized: AGC(target={:.3f}, max_gain={:.1f}dB), "
                "RNNoise(level={:.1f}), swresample 48k→8k",
                cfg_.agc_target_rms, cfg_.agc_max_gain, cfg_.denoise_level);
    return true;
}

bool AudioProcessor::process(const int16_t* input, size_t input_samples,
                             int16_t* output, size_t& output_samples) {
    if (input_samples != 480) {
        SPDLOG_ERROR("AudioProcessor: expected 480 input samples, got {}", input_samples);
        return false;
    }

    // Copy input to working buffer (float)
    float work[480];
    for (size_t i = 0; i < 480; i++) {
        work[i] = static_cast<float>(input[i]);
    }

    // Stage 1: AGC
    {
        // Copy to int16 for AGC
        int16_t agc_buf[480];
        for (int i = 0; i < 480; i++) {
            agc_buf[i] = static_cast<int16_t>(
                std::clamp(work[i], -32768.0f, 32767.0f));
        }
        agc_process(agc_buf, 480);
        for (int i = 0; i < 480; i++) {
            work[i] = static_cast<float>(agc_buf[i]);
        }
    }

    // Stage 2: RNNoise denoise
    {
        float rnnoise_out[480];
        last_vad_ = rnnoise_process_frame(rnnoise_state_, rnnoise_out, work);

        // Soft mix: denoise_level controls blend between original and processed
        float level = std::clamp(cfg_.denoise_level, 0.0f, 1.0f);
        if (level < 1.0f) {
            for (int i = 0; i < 480; i++) {
                work[i] = level * rnnoise_out[i] + (1.0f - level) * work[i];
            }
        } else {
            std::memcpy(work, rnnoise_out, sizeof(work));
        }
    }

    // Stage 3: swresample 48kHz → 8kHz (int16 → int16)
    {
        // Convert float (int16-range) to int16 for swresample
        int16_t src[480];
        for (int i = 0; i < 480; i++) {
            int32_t v = static_cast<int32_t>(std::round(work[i]));
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            src[i] = static_cast<int16_t>(v);
        }

        const uint8_t* in_buf[1] = { (const uint8_t*)src };
        uint8_t* out_buf[1] = { (uint8_t*)output };
        int ret = swr_convert(swr_, out_buf, 80, in_buf, 480);
        if (ret < 0) {
            SPDLOG_ERROR("AudioProcessor: swr_convert failed: {}", ret);
            output_samples = 0;
            return false;
        }
        output_samples = static_cast<size_t>(ret);
    }

    return true;
}

void AudioProcessor::agc_process(int16_t* samples, size_t count) {
    // Compute RMS
    float sum_sq = 0.0f;
    for (size_t i = 0; i < count; i++) {
        float s = static_cast<float>(samples[i]) / 32768.0f;
        sum_sq += s * s;
    }
    float rms = std::sqrt(sum_sq / static_cast<float>(count));
    rms = std::max(rms, 1e-6f);

    // Desired gain
    float target_rms = cfg_.agc_target_rms;
    float max_gain = cfg_.agc_max_gain;
    float desired = target_rms / rms;
    desired = std::min(desired, max_gain);
    desired = std::max(desired, 1.0f); // never attenuate

    // Smooth: use attack coeff if gain increasing, release coeff if decreasing
    float coeff = (desired > agc_current_gain_) ? agc_attack_coeff_ : agc_release_coeff_;
    agc_current_gain_ = coeff * agc_current_gain_ + (1.0f - coeff) * desired;

    // Apply gain with soft clip
    for (size_t i = 0; i < count; i++) {
        float s = static_cast<float>(samples[i]) * agc_current_gain_;
        if (s > 32767.0f) s = 32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        samples[i] = static_cast<int16_t>(s);
    }
}

void AudioProcessor::set_denoise_level(float level) {
    cfg_.denoise_level = std::clamp(level, 0.0f, 1.0f);
    SPDLOG_DEBUG("AudioProcessor: denoise_level = {:.2f}", cfg_.denoise_level);
}

void AudioProcessor::set_agc_target(float level) {
    cfg_.agc_target_rms = std::clamp(level, 0.001f, 1.0f);
    SPDLOG_DEBUG("AudioProcessor: agc_target_rms = {:.4f}", cfg_.agc_target_rms);
}

} // namespace smartcam
