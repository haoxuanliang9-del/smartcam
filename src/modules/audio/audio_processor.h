#ifndef SMARTCAM_MODULES_AUDIO_AUDIO_PROCESSOR_H
#define SMARTCAM_MODULES_AUDIO_AUDIO_PROCESSOR_H

#include "common/config.h"
#include <cstdint>
#include <cstddef>
#include <memory>

struct DenoiseState;

namespace smartcam {

class AudioProcessor {
public:
    AudioProcessor() = default;
    ~AudioProcessor();

    AudioProcessor(const AudioProcessor&) = delete;
    AudioProcessor& operator=(const AudioProcessor&) = delete;

    bool init(const AudioEnhanceConfig& cfg);

    // Process one 10ms frame:
    //   input:  480 samples @ 48kHz, int16 mono
    //   output:  80 samples @ 8kHz,  int16 mono
    // Returns true on success.
    bool process(const int16_t* input, size_t input_samples,
                 int16_t* output, size_t& output_samples);

    bool is_enabled() const { return enabled_; }

    void set_denoise_level(float level);
    void set_agc_target(float level);

private:
    void agc_process(int16_t* samples, size_t count);
    void rnnoise_process(float* frame, size_t count);
    void resample_48k_to_8k(const float* in, size_t in_count,
                            int16_t* out, size_t& out_count);

    AudioEnhanceConfig cfg_;
    bool enabled_ = false;
    DenoiseState* rnnoise_state_ = nullptr;

    // AGC state
    float agc_current_gain_ = 1.0f;
    float agc_attack_coeff_ = 0.0f;
    float agc_release_coeff_ = 0.0f;
};

} // namespace smartcam

#endif // SMARTCAM_MODULES_AUDIO_AUDIO_PROCESSOR_H
