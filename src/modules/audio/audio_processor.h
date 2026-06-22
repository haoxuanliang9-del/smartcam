#ifndef SMARTCAM_MODULES_AUDIO_AUDIO_PROCESSOR_H
#define SMARTCAM_MODULES_AUDIO_AUDIO_PROCESSOR_H

#include "common/config.h"
#include <cstdint>
#include <cstddef>

struct DenoiseState;
struct SwrContext;

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

    void set_denoise_level(float level);
    void set_agc_target(float level);

private:
    void agc_process(int16_t* samples, size_t count);

    AudioEnhanceConfig cfg_;
    DenoiseState* rnnoise_state_ = nullptr;
    SwrContext* swr_ = nullptr;

    // AGC state
    float agc_current_gain_ = 1.0f;
    float agc_attack_coeff_ = 0.0f;
    float agc_release_coeff_ = 0.0f;
};

} // namespace smartcam

#endif // SMARTCAM_MODULES_AUDIO_AUDIO_PROCESSOR_H
