#include "audio_processor.h"
#include <spdlog/spdlog.h>
#include <rnnoise.h>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace smartcam {

AudioProcessor::~AudioProcessor() {
    if (rnnoise_state_) {
        rnnoise_destroy(rnnoise_state_);
        rnnoise_state_ = nullptr;
    }
}

bool AudioProcessor::init(const AudioEnhanceConfig& cfg) {
    cfg_ = cfg;
    enabled_ = cfg_.enabled;

    if (!enabled_) {
        SPDLOG_INFO("AudioProcessor: disabled, will bypass");
        return true;
    }

    // Init RNNoise
    rnnoise_state_ = rnnoise_create(nullptr); // nullptr = use built-in model
    if (!rnnoise_state_) {
        SPDLOG_ERROR("AudioProcessor: rnnoise_create failed");
        enabled_ = false;
        return false;
    }

    if (rnnoise_get_frame_size() != 480) {
        SPDLOG_ERROR("AudioProcessor: unexpected RNNoise frame size {}",
                     rnnoise_get_frame_size());
        rnnoise_destroy(rnnoise_state_);
        rnnoise_state_ = nullptr;
        enabled_ = false;
        return false;
    }

    // Init AGC smoothing coefficients
    constexpr float frame_ms = 10.0f;
    agc_attack_coeff_ = std::exp(-frame_ms / cfg_.agc_attack_ms);
    agc_release_coeff_ = std::exp(-frame_ms / cfg_.agc_release_ms);
    agc_current_gain_ = 1.0f;

    SPDLOG_INFO("AudioProcessor initialized: AGC(target={:.3f}, max_gain={:.1f}dB), "
                "RNNoise(level={:.1f})",
                cfg_.agc_target_rms, cfg_.agc_max_gain, cfg_.denoise_level);
    return true;
}

bool AudioProcessor::process(const int16_t* input, size_t input_samples,
                             int16_t* output, size_t& output_samples) {
    if (input_samples != 480) {
        SPDLOG_ERROR("AudioProcessor: expected 480 input samples, got {}", input_samples);
        return false;
    }

    if (!enabled_) {
        // Bypass: output silence (shouldn't happen in normal operation —
        // AudioCapture won't call process() when disabled)
        std::memset(output, 0, 80 * sizeof(int16_t));
        output_samples = 80;
        return true;
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
        rnnoise_process_frame(rnnoise_state_, rnnoise_out, work);

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

    // Stage 3: Resample 48kHz → 8kHz
    resample_48k_to_8k(work, 480, output, output_samples);

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

void AudioProcessor::resample_48k_to_8k(const float* in, size_t in_count,
                                         int16_t* out, size_t& out_count) {
    // Two-stage decimation: 48kHz → 24kHz (÷2) → 8kHz (÷3)
    // Stage 1: half-band FIR + decimate by 2
    // 7-tap half-band filter, cutoff ≈ 12kHz
    static constexpr float hb_coeff[7] = {
        -0.03125f, 0.0f, 0.28125f, 0.5f, 0.28125f, 0.0f, -0.03125f
    };

    float stage1[240];
    for (int i = 0; i < 240; i++) {
        float sum = 0.0f;
        for (int j = 0; j < 7; j++) {
            int idx = 2 * i - j + 3;
            if (idx >= 0 && idx < static_cast<int>(in_count)) {
                sum += in[idx] * hb_coeff[j];
            }
        }
        stage1[i] = sum;
    }

    // Stage 2: 3-tap anti-alias filter + decimate by 3
    // Simple triangular filter [1, 2, 1]/4, cutoff ≈ 4kHz
    out_count = 80;
    for (int i = 0; i < 80; i++) {
        int base = i * 3;
        float s0 = (base - 1 >= 0)            ? stage1[base - 1] : 0.0f;
        float s1 = stage1[base];
        float s2 = (base + 1 < 240)           ? stage1[base + 1] : 0.0f;
        float sum = (s0 + 2.0f * s1 + s2) / 4.0f;

        // Convert to int16 with clamping
        int32_t val = static_cast<int32_t>(sum);
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        out[i] = static_cast<int16_t>(val);
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
