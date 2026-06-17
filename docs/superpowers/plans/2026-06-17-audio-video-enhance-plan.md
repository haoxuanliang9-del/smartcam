# Audio & Video Enhancement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add real-time audio noise suppression (AGC + RNNoise) and video low-light enhancement (CLAHE + NLMeans denoising) to SmartCam's capture pipelines.

**Architecture:** Two independent processor modules (AudioProcessor, VideoProcessor) inserted into the existing capture pipelines. AudioProcessor wraps RNNoise (pure C, vendored) with a pre-AGC stage and a 48k→8k resampler. VideoProcessor wraps OpenCV CLAHE and fastNlMeansDenoising. Both are toggled via `config.yaml` and compile/enable independently.

**Tech Stack:** C++17, RNNoise (vendored C), OpenCV (core+imgproc+photo, optional), CMake 3.16+

## Global Constraints

- Target hardware: Orange Pi Zero 3 (ARM Cortex-A53, quad-core, 1-1.5GB RAM)
- Dev platform: x86 Linux VM
- Backward compatibility: `enabled: false` in config must produce identical behavior to v1.0.0
- OpenCV is optional at compile time — if not found, VideoProcessor compiles as a no-op stub
- RNNoise is vendored in `third_party/rnnoise/` — zero external audio dependencies beyond ALSA
- Audio pipeline: 48kHz in → AGC → RNNoise → resample 48k→8k → noise gate → PCMU
- Video pipeline: YUV420P → CLAHE (Y-channel only) → fastNlMeansDenoising (skip-frame) → OSD → H.264
- Each processor has a single `process()` entry point with no side effects on RTSP or encoding state

## File Structure Map

```
New files:
  third_party/rnnoise/rnnoise.h              — Public API header (vendored)
  third_party/rnnoise/denoise.c              — Core denoising (vendored)
  third_party/rnnoise/rnn.c                  — GRU RNN computation (vendored)
  third_party/rnnoise/rnn.h                  — RNN types (vendored)
  third_party/rnnoise/rnn_data.c             — Built-in model weights (vendored)
  third_party/rnnoise/rnn_data.h             — Weight declarations (vendored)
  third_party/rnnoise/pitch.c                — Pitch analysis (vendored)
  third_party/rnnoise/pitch.h                — Pitch types (vendored)
  third_party/rnnoise/celt_lpc.c             — LPC analysis (vendored)
  third_party/rnnoise/celt_lpc.h             — LPC declarations (vendored)
  third_party/rnnoise/kiss_fft.c             — KISS FFT (vendored)
  third_party/rnnoise/kiss_fft.h             — FFT declarations (vendored)
  third_party/rnnoise/_kiss_fft_guts.h       — FFT internals (vendored)
  third_party/rnnoise/common.h               — OPUS_* macros (vendored)
  src/modules/audio/audio_processor.h        — AudioProcessor interface
  src/modules/audio/audio_processor.cpp      — AGC + RNNoise wrapper + resampler
  src/modules/video/video_processor.h        — VideoProcessor interface
  src/modules/video/video_processor.cpp      — CLAHE + NLMeans denoise + skip-frame

Modified files:
  src/common/config.h                        — +AudioEnhanceConfig, +VideoEnhanceConfig
  src/common/config.cpp                      — +YAML parsing for new sections
  src/modules/audio/audio_capture.h          — +set_audio_processor(), +48kHz mode members
  src/modules/audio/audio_capture.cpp        — +48kHz capture path with AudioProcessor
  src/modules/camera/camera_capture.h        — +set_video_processor(), +VideoProcessor member
  src/modules/camera/camera_capture.cpp      — +VideoProcessor call before OSD/encode
  src/app/main_service.h                     — +processor shared_ptr members
  src/app/main_service.cpp                   — +processor creation & injection
  CMakeLists.txt                             — +rnnoise lib, +opencv optional, +new sources
```

---

### Task 1: RNNoise Third-Party Library

**Files:**
- Create: `third_party/rnnoise/rnnoise.h`, `third_party/rnnoise/common.h`
- Create: `third_party/rnnoise/denoise.c`, `third_party/rnnoise/rnn.c`, `third_party/rnnoise/rnn_data.c`
- Create: `third_party/rnnoise/pitch.c`, `third_party/rnnoise/celt_lpc.c`, `third_party/rnnoise/kiss_fft.c`
- Create: `third_party/rnnoise/rnn.h`, `third_party/rnnoise/rnn_data.h`, `third_party/rnnoise/pitch.h`
- Create: `third_party/rnnoise/celt_lpc.h`, `third_party/rnnoise/kiss_fft.h`, `third_party/rnnoise/_kiss_fft_guts.h`
- Modify: `CMakeLists.txt` — add rnnoise static library target

**Interfaces:**
- Produces: `rnnoise_create(const RNNModel*) → DenoiseState*`, `rnnoise_process_frame(DenoiseState*, float*, const float*) → float`, `rnnoise_destroy(DenoiseState*)`, `rnnoise_get_frame_size() → int`
- Produces: CMake target `rnnoise` (static library)

- [ ] **Step 1: Download RNNoise source from upstream**

Clone the official RNNoise repository and copy source files into `third_party/rnnoise/`. The upstream is at `https://github.com/xiph/rnnoise`. We only need the core C files — no autotools, no examples, no training scripts.

Run:
```bash
cd /tmp && git clone --depth 1 https://github.com/xiph/rnnoise.git
cd "C:\共享文件夹\projects\smartcam"
mkdir -p third_party/rnnoise
cp /tmp/rnnoise/src/rnnoise.h third_party/rnnoise/
cp /tmp/rnnoise/src/common.h third_party/rnnoise/
cp /tmp/rnnoise/src/denoise.c third_party/rnnoise/
cp /tmp/rnnoise/src/rnn.c third_party/rnnoise/
cp /tmp/rnnoise/src/rnn.h third_party/rnnoise/
cp /tmp/rnnoise/src/rnn_data.c third_party/rnnoise/
cp /tmp/rnnoise/src/rnn_data.h third_party/rnnoise/
cp /tmp/rnnoise/src/pitch.c third_party/rnnoise/
cp /tmp/rnnoise/src/pitch.h third_party/rnnoise/
cp /tmp/rnnoise/src/celt_lpc.c third_party/rnnoise/
cp /tmp/rnnoise/src/celt_lpc.h third_party/rnnoise/
cp /tmp/rnnoise/src/kiss_fft.c third_party/rnnoise/
cp /tmp/rnnoise/src/kiss_fft.h third_party/rnnoise/
cp /tmp/rnnoise/src/_kiss_fft_guts.h third_party/rnnoise/
rm -rf /tmp/rnnoise
```

- [ ] **Step 2: Verify required symbols exist in vendored files**

Run:
```bash
cd "C:\共享文件夹\projects\smartcam"
grep -n "rnnoise_create\|rnnoise_process_frame\|rnnoise_destroy\|rnnoise_get_frame_size\|RNNModel\|DenoiseState" third_party/rnnoise/rnnoise.h
```

Expected: The header must declare `DenoiseState`, `RNNModel`, `rnnoise_create()`, `rnnoise_process_frame()`, `rnnoise_destroy()`, and `rnnoise_get_frame_size()`.

- [ ] **Step 3: Add RNNoise to CMakeLists.txt**

Edit `CMakeLists.txt`. Add after the `include_directories(...)` block (line 33):

```cmake
# RNNoise — neural noise suppression (vendored, always compiled)
add_library(rnnoise STATIC
    third_party/rnnoise/denoise.c
    third_party/rnnoise/rnn.c
    third_party/rnnoise/rnn_data.c
    third_party/rnnoise/pitch.c
    third_party/rnnoise/celt_lpc.c
    third_party/rnnoise/kiss_fft.c
)
target_include_directories(rnnoise PUBLIC third_party/rnnoise)
target_link_libraries(rnnoise PUBLIC m)
```

Also add `rnnoise` to the smartcam target's link libraries. Modify the `target_link_libraries(smartcam ...)` block to add `rnnoise`:

```cmake
target_link_libraries(smartcam
    yaml-cpp
    spdlog::spdlog
    fmt
    pthread
    rnnoise
    PkgConfig::ALSA
)
```

- [ ] **Step 4: Build and verify RNNoise compiles**

Run:
```bash
cd "C:\共享文件夹\projects\smartcam"
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target rnnoise -j$(nproc)
```

Expected: rnnoise static library builds without errors.

- [ ] **Step 5: Verify full smartcam still builds**

Run:
```bash
cmake --build . -j$(nproc)
```

Expected: smartcam binary links successfully (RNNoise is unused at this point, just linked).

- [ ] **Step 6: Commit**

```bash
git add third_party/rnnoise/ CMakeLists.txt
git commit -m "build: vendor RNNoise neural noise suppression library"
```

---

### Task 2: Enhancement Config Structs

**Files:**
- Modify: `src/common/config.h` — add `AudioEnhanceConfig` and `VideoEnhanceConfig` structs, add members to `Config`
- Modify: `src/common/config.cpp` — add YAML parsing for `audio_enhance` and `video_enhance` sections
- Create: `config/config.yaml.example` — document new config sections (optional reference)

**Interfaces:**
- Produces: `smartcam::AudioEnhanceConfig` struct with fields: `enabled`, `agc_target_rms`, `agc_max_gain`, `agc_attack_ms`, `agc_release_ms`, `denoise_level`, `rnnoise_model`
- Produces: `smartcam::VideoEnhanceConfig` struct with fields: `enabled`, `clahe_clip_limit`, `clahe_tile_size`, `denoise_h`, `denoise_skip_frames`
- Produces: `Config::audio_enhance` and `Config::video_enhance` members

- [ ] **Step 1: Add struct declarations to config.h**

Edit `src/common/config.h`. Insert after the `DisplayConfig` struct (before the `Config` struct):

```cpp
struct AudioEnhanceConfig {
    bool enabled = false;
    float agc_target_rms = 0.1f;       // -20dBFS
    float agc_max_gain = 30.0f;        // +30dB max
    float agc_attack_ms = 5.0f;
    float agc_release_ms = 100.0f;
    float denoise_level = 1.0f;        // 0.0=bypass, 1.0=max
    std::string rnnoise_model;         // empty = built-in model
};

struct VideoEnhanceConfig {
    bool enabled = false;
    float clahe_clip_limit = 2.0f;
    int clahe_tile_size = 8;
    float denoise_h = 10.0f;           // 0 = bypass denoise
    int denoise_skip_frames = 2;       // process every Nth frame
};
```

Then add two new members to the `Config` struct, before the closing `};`:

```cpp
    AudioEnhanceConfig audio_enhance;
    VideoEnhanceConfig video_enhance;
```

- [ ] **Step 2: Add YAML parsing to config.cpp**

Edit `src/common/config.cpp`. Insert after the `display` section parsing block (after the closing `}` of the `if (root["display"])` block, around line 61):

```cpp
        if (root["audio_enhance"]) {
            auto n = root["audio_enhance"];
            if (n["enabled"]) audio_enhance.enabled = n["enabled"].as<bool>();
            if (n["agc_target_rms"]) audio_enhance.agc_target_rms = n["agc_target_rms"].as<float>();
            if (n["agc_max_gain"]) audio_enhance.agc_max_gain = n["agc_max_gain"].as<float>();
            if (n["agc_attack_ms"]) audio_enhance.agc_attack_ms = n["agc_attack_ms"].as<float>();
            if (n["agc_release_ms"]) audio_enhance.agc_release_ms = n["agc_release_ms"].as<float>();
            if (n["denoise_level"]) audio_enhance.denoise_level = n["denoise_level"].as<float>();
            if (n["rnnoise_model"]) audio_enhance.rnnoise_model = n["rnnoise_model"].as<std::string>();
        }

        if (root["video_enhance"]) {
            auto n = root["video_enhance"];
            if (n["enabled"]) video_enhance.enabled = n["enabled"].as<bool>();
            if (n["clahe_clip_limit"]) video_enhance.clahe_clip_limit = n["clahe_clip_limit"].as<float>();
            if (n["clahe_tile_size"]) video_enhance.clahe_tile_size = n["clahe_tile_size"].as<int>();
            if (n["denoise_h"]) video_enhance.denoise_h = n["denoise_h"].as<float>();
            if (n["denoise_skip_frames"]) video_enhance.denoise_skip_frames = n["denoise_skip_frames"].as<int>();
        }
```

- [ ] **Step 3: Build and verify config loads without errors**

Run:
```bash
cd "C:\共享文件夹\projects\smartcam\build"
cmake .. && cmake --build . -j$(nproc)
```

Expected: Compiles. To verify parsing, add a temporary debug log in config.cpp after the new blocks:
```cpp
SPDLOG_INFO("Audio enhance enabled: {}, Video enhance enabled: {}",
            audio_enhance.enabled, video_enhance.enabled);
```

Create a test config at `/tmp/test_config.yaml`:
```yaml
audio_enhance:
  enabled: true
video_enhance:
  enabled: false
```

Run:
```bash
./smartcam -c /tmp/test_config.yaml
```

Expected output: `Audio enhance enabled: true, Video enhance enabled: false`

Remove the temporary debug log after verification.

- [ ] **Step 4: Commit**

```bash
git add src/common/config.h src/common/config.cpp
git commit -m "feat: add AudioEnhanceConfig and VideoEnhanceConfig structs with YAML parsing"
```

---

### Task 3: AudioProcessor Module

**Files:**
- Create: `src/modules/audio/audio_processor.h`
- Create: `src/modules/audio/audio_processor.cpp`
- Modify: `CMakeLists.txt` — add new source files

**Interfaces:**
- Consumes: `AudioEnhanceConfig` from Task 2, RNNoise C API from Task 1
- Produces: `AudioProcessor::init(const AudioEnhanceConfig&) → bool`
- Produces: `AudioProcessor::process(const int16_t* input, size_t input_samples, int16_t* output, size_t& output_samples) → bool`
- Produces: `AudioProcessor::is_enabled() → bool`
- Produces: `AudioProcessor::set_denoise_level(float) → void`
- Produces: `AudioProcessor::set_agc_target(float) → void`

The input is always 480 samples (10ms @ 48kHz). The output is always 80 samples (10ms @ 8kHz). The module has three internal stages: AGC → RNNoise → Resampler.

- [ ] **Step 1: Create audio_processor.h**

Write `src/modules/audio/audio_processor.h`:

```cpp
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
```

- [ ] **Step 2: Create audio_processor.cpp**

Write `src/modules/audio/audio_processor.cpp`:

```cpp
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
        // Bypass: just copy (shouldn't happen in normal operation —
        // AudioCapture won't call process() when disabled)
        std::memcpy(output, input, input_samples * sizeof(int16_t));
        output_samples = input_samples;
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
        float vad = rnnoise_process_frame(rnnoise_state_, rnnoise_out, work);

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

void AudioProcessor::rnnoise_process(float* frame, size_t count) {
    // RNNoise processes in-place via rnnoise_process_frame
    // Already called in process() above — this is a no-op at this level.
    // The actual RNNoise call is in process() for clarity.
    (void)frame;
    (void)count;
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

        // Convert float (-1..1) to int16
        float scaled = sum / 32768.0f; // RNNoise output is already int16-scale
        // Actually stage1 values are in int16 range (-32768..32767),
        // so convert directly
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
```

- [ ] **Step 3: Add AudioProcessor sources to CMakeLists.txt**

Edit `CMakeLists.txt`. In the `SMARTCAM_SOURCES` list, add after `src/modules/audio/audio_capture.cpp`:

```cmake
    src/modules/audio/audio_processor.cpp
```

- [ ] **Step 4: Build and verify compilation**

Run:
```bash
cd "C:\共享文件夹\projects\smartcam\build"
cmake .. && cmake --build . -j$(nproc)
```

Expected: Compiles without errors. AudioProcessor is not instantiated yet, so linking succeeds even though no one calls it.

- [ ] **Step 5: Commit**

```bash
git add src/modules/audio/audio_processor.h src/modules/audio/audio_processor.cpp CMakeLists.txt
git commit -m "feat: add AudioProcessor module (AGC + RNNoise + 48k→8k resampler)"
```

---

### Task 4: AudioCapture Enhancement Integration

**Files:**
- Modify: `src/modules/audio/audio_capture.h` — add `set_audio_processor()`, 48kHz mode members
- Modify: `src/modules/audio/audio_capture.cpp` — add 48kHz ALSA init path, processor call in capture_loop

**Interfaces:**
- Consumes: `AudioProcessor` from Task 3
- Produces: `AudioCapture::set_audio_processor(std::shared_ptr<AudioProcessor>) → void`
- Modified: `AudioCapture::capture_loop()` — dual-path (48kHz enhanced vs 8kHz legacy)

The key change: when an AudioProcessor is set and enabled, AudioCapture opens ALSA at 48kHz instead of 8kHz, captures 480-sample frames, calls `audio_processor_->process()`, then feeds the resulting 8kHz PCM through the existing noise gate + PCMU pipeline.

- [ ] **Step 1: Modify audio_capture.h**

Edit `src/modules/audio/audio_capture.h`. Add `#include <memory>` (already present) and forward declare `AudioProcessor`. Add public method and private members.

After the `set_volume()` / `volume()` methods, add:

```cpp
    // Inject optional audio processor (AGC + RNNoise + resample).
    // When set and enabled, ALSA opens at 48kHz; processor converts to 8kHz.
    void set_audio_processor(std::shared_ptr<class AudioProcessor> processor) {
        audio_processor_ = std::move(processor);
    }
```

In the private section, after `std::thread capture_thread_;`, add:

```cpp
    // Enhanced audio path (optional)
    std::shared_ptr<class AudioProcessor> audio_processor_;
    bool use_enhanced_path_ = false;
```

- [ ] **Step 2: Modify audio_capture.cpp — init()**

Edit `src/modules/audio/audio_capture.cpp`. Add include at top:

```cpp
#include "audio_processor.h"
```

Modify `AudioCapture::init()`. The current implementation hardcodes `rate = 8000` and `period_size = 160`. Change to support dual sample rates:

```cpp
bool AudioCapture::init(const std::string& device) {
    device_ = device;

    // Determine sample rate: 48kHz if enhanced path, else 8kHz
    unsigned int rate;
    snd_pcm_uframes_t period_size;

    if (audio_processor_ && audio_processor_->is_enabled()) {
        rate = 48000;
        period_size = 480; // 10ms @ 48kHz
        use_enhanced_path_ = true;
        SPDLOG_INFO("AudioCapture: using 48kHz enhanced path");
    } else {
        rate = 8000;
        period_size = 160; // 20ms @ 8kHz
        use_enhanced_path_ = false;
        SPDLOG_INFO("AudioCapture: using 8kHz legacy path");
    }

    int err = snd_pcm_open(&pcm_, device_.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        SPDLOG_ERROR("AudioCapture: cannot open ALSA device '{}': {}", device_, snd_strerror(err));
        use_enhanced_path_ = false;
        return false;
    }

    snd_pcm_hw_params_t* hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm_, hw_params);

    snd_pcm_hw_params_set_access(pcm_, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_, hw_params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm_, hw_params, 1);

    snd_pcm_hw_params_set_rate_near(pcm_, hw_params, &rate, nullptr);

    snd_pcm_uframes_t period_near = period_size;
    snd_pcm_hw_params_set_period_size_near(pcm_, hw_params, &period_near, nullptr);

    snd_pcm_uframes_t buffer_size = period_near * 4;
    snd_pcm_hw_params_set_buffer_size_near(pcm_, hw_params, &buffer_size);

    err = snd_pcm_hw_params(pcm_, hw_params);
    if (err < 0) {
        SPDLOG_ERROR("AudioCapture: cannot set hw params: {}", snd_strerror(err));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        use_enhanced_path_ = false;
        return false;
    }

    snd_pcm_prepare(pcm_);

    SPDLOG_INFO("AudioCapture initialized: device={}, rate={}, channels=1, period={}",
                device_, rate, period_near);
    return true;
}
```

- [ ] **Step 3: Modify audio_capture.cpp — capture_loop()**

Replace the `capture_loop()` method. The new version has two paths:

```cpp
void AudioCapture::capture_loop() {
    if (use_enhanced_path_) {
        // ── Enhanced path: 48kHz capture → AudioProcessor → 8kHz → noise gate → PCMU ──
        const int period_frames_48k = 480;   // 10ms @ 48kHz
        const int period_frames_8k = 80;     // 10ms @ 8kHz
        std::vector<int16_t> pcm_48k(period_frames_48k);
        std::vector<int16_t> pcm_8k(period_frames_8k);
        std::vector<uint8_t> ulaw_buf(period_frames_8k);

        const uint32_t sample_rate = 8000;
        uint64_t total_samples = 0;

        while (running_) {
            snd_pcm_sframes_t frames = snd_pcm_readi(pcm_, pcm_48k.data(), period_frames_48k);

            if (frames < 0) {
                frames = snd_pcm_recover(pcm_, frames, 0);
                if (frames < 0) {
                    SPDLOG_ERROR("AudioCapture: ALSA read failed: {}", snd_strerror(frames));
                    break;
                }
                continue;
            }
            if (frames == 0) continue;

            // Processor: 48kHz → AGC → RNNoise → resample → 8kHz
            size_t out_samples = 0;
            if (!audio_processor_->process(pcm_48k.data(), period_frames_48k,
                                           pcm_8k.data(), out_samples)) {
                SPDLOG_ERROR("AudioCapture: processor failed");
                continue;
            }

            // Noise gate (threshold lowered to 300 since RNNoise handles noise now)
            const int16_t noise_gate = 300;
            int16_t peak = 0;
            for (size_t i = 0; i < out_samples; i++) {
                int16_t a = pcm_8k[i] >= 0 ? pcm_8k[i] : -pcm_8k[i];
                if (a > peak) peak = a;
            }
            if (peak < noise_gate) {
                std::fill(pcm_8k.begin(), pcm_8k.begin() + out_samples, 0);
            }

            // PCMU encode
            for (size_t i = 0; i < out_samples; i++) {
                ulaw_buf[i] = pcmu_encode(pcm_8k[i]);
            }

            // Timestamp from cumulative samples
            uint64_t timestamp = (total_samples * 1000000ULL) / sample_rate;
            total_samples += static_cast<uint64_t>(out_samples);

            AudioFrame frame;
            frame.timestamp = timestamp;
            frame.data.assign(ulaw_buf.begin(), ulaw_buf.begin() + out_samples);

            auto shared_frame = std::make_shared<AudioFrame>(std::move(frame));
            {
                std::lock_guard<std::mutex> lock(queues_mutex_);
                for (auto& slot : client_slots_) {
                    slot->put(shared_frame);
                }
            }
        }
    } else {
        // ── Legacy path: 8kHz capture → gain → noise gate → PCMU (unchanged) ──
        const int period_frames = 160;
        std::vector<int16_t> pcm_buf(period_frames);
        std::vector<uint8_t> ulaw_buf(period_frames);

        uint64_t total_samples = 0;
        const uint32_t sample_rate = 8000;

        while (running_) {
            snd_pcm_sframes_t frames = snd_pcm_readi(pcm_, pcm_buf.data(), period_frames);

            if (frames < 0) {
                frames = snd_pcm_recover(pcm_, frames, 0);
                if (frames < 0) {
                    SPDLOG_ERROR("AudioCapture: ALSA read failed: {}", snd_strerror(frames));
                    break;
                }
                continue;
            }

            if (frames == 0) continue;

            float vol = volume_.load();
            if (vol != 1.0f) {
                for (snd_pcm_sframes_t i = 0; i < frames; i++) {
                    int32_t s = static_cast<int32_t>(pcm_buf[i] * vol);
                    if (s > 32767) s = 32767;
                    if (s < -32768) s = -32768;
                    pcm_buf[i] = static_cast<int16_t>(s);
                }
            }

            const int16_t noise_gate = 1500;
            int16_t peak = 0;
            for (snd_pcm_sframes_t i = 0; i < frames; i++) {
                int16_t a = pcm_buf[i] >= 0 ? pcm_buf[i] : -pcm_buf[i];
                if (a > peak) peak = a;
            }
            if (peak < noise_gate) {
                std::fill(pcm_buf.begin(), pcm_buf.begin() + frames, 0);
            }

            for (snd_pcm_sframes_t i = 0; i < frames; i++) {
                ulaw_buf[i] = pcmu_encode(pcm_buf[i]);
            }

            uint64_t timestamp = (total_samples * 1000000ULL) / sample_rate;
            total_samples += static_cast<uint64_t>(frames);

            AudioFrame frame;
            frame.timestamp = timestamp;
            frame.data.assign(ulaw_buf.begin(), ulaw_buf.begin() + frames);

            auto shared_frame = std::make_shared<AudioFrame>(std::move(frame));

            {
                std::lock_guard<std::mutex> lock(queues_mutex_);
                for (auto& slot : client_slots_) {
                    slot->put(shared_frame);
                }
            }
        }
    }
}
```

- [ ] **Step 4: Build and verify compilation**

Run:
```bash
cd "C:\共享文件夹\projects\smartcam\build"
cmake .. && cmake --build . -j$(nproc)
```

Expected: Compiles without errors. The legacy path is still the default (no processor injected), so behavior is unchanged.

- [ ] **Step 5: Commit**

```bash
git add src/modules/audio/audio_capture.h src/modules/audio/audio_capture.cpp
git commit -m "feat: integrate AudioProcessor into AudioCapture with dual-rate (48k/8k) pipeline"
```

---

### Task 5: VideoProcessor Module

**Files:**
- Create: `src/modules/video/video_processor.h`
- Create: `src/modules/video/video_processor.cpp`
- Modify: `CMakeLists.txt` — add OpenCV optional dependency and new sources

**Interfaces:**
- Consumes: `VideoEnhanceConfig` from Task 2, OpenCV (core + imgproc + photo)
- Produces: `VideoProcessor::init(const VideoEnhanceConfig&) → bool`
- Produces: `VideoProcessor::process(uint8_t* y, uint8_t* u, uint8_t* v, int w, int h, int y_stride, int uv_stride) → bool`
- Produces: `VideoProcessor::is_enabled() → bool`

When `HAS_OPENCV` is not defined, the entire implementation is stubbed out (process() is a no-op that returns true).

- [ ] **Step 1: Create video_processor.h**

Write `src/modules/video/video_processor.h`:

```cpp
#ifndef SMARTCAM_MODULES_VIDEO_VIDEO_PROCESSOR_H
#define SMARTCAM_MODULES_VIDEO_VIDEO_PROCESSOR_H

#include "common/config.h"
#include <cstdint>

#ifdef HAS_OPENCV
#include <opencv2/core.hpp>
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
```

- [ ] **Step 2: Create video_processor.cpp**

Write `src/modules/video/video_processor.cpp`:

```cpp
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

    // ── CLAHE on Y channel ──
    cv::Mat y_channel(height, width, CV_8UC1, y_data, y_stride);
    clahe_->apply(y_channel, y_channel);

    // ── NLMeans denoise (skip-frame) ──
    if (cfg_.denoise_h > 0.0f) {
        int skip = cfg_.denoise_skip_frames + 1; // skip_frames=2 → every 3rd frame
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
            cv::Mat y_denoised, u_denoised_full, v_denoised_full;
            cv::Mat split_channels[3] = {y_denoised, u_denoised_full, v_denoised_full};
            cv::split(yuv_denoised, split_channels);

            // Write Y back (full res)
            std::memcpy(y_data, y_denoised.data, width * height);

            // Downsample UV back to half-res and write
            cv::Mat u_half_out(height / 2, width / 2, CV_8UC1);
            cv::Mat v_half_out(height / 2, width / 2, CV_8UC1);
            cv::resize(u_denoised_full, u_half_out, cv::Size(width / 2, height / 2), 0, 0, cv::INTER_LINEAR);
            cv::resize(v_denoised_full, v_half_out, cv::Size(width / 2, height / 2), 0, 0, cv::INTER_LINEAR);
            std::memcpy(u_data, u_half_out.data, (width / 2) * (height / 2));
            std::memcpy(v_data, v_half_out.data, (width / 2) * (height / 2));
        }
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
```

- [ ] **Step 3: Add VideoProcessor to CMakeLists.txt with OpenCV guard**

Edit `CMakeLists.txt`. Add before the `SMARTCAM_SOURCES` block:

```cmake
# OpenCV — optional, for video enhancement
option(ENABLE_VIDEO_ENHANCE "Enable OpenCV-based video enhancement" ON)
if(ENABLE_VIDEO_ENHANCE)
    find_package(OpenCV QUIET COMPONENTS core imgproc photo)
    if(OpenCV_FOUND)
        add_compile_definitions(HAS_OPENCV)
        message(STATUS "OpenCV found: video enhancement enabled")
    else()
        message(STATUS "OpenCV NOT found: video enhancement will be a no-op stub")
    endif()
endif()
```

In the `SMARTCAM_SOURCES` list, add after `src/modules/camera/camera_capture.cpp`:

```cmake
    src/modules/video/video_processor.cpp
```

In the `target_link_libraries(smartcam ...)` block, add OpenCV libraries conditionally. After the existing `target_link_libraries(smartcam ...)` call, add:

```cmake
if(OpenCV_FOUND)
    target_link_libraries(smartcam ${OpenCV_LIBS})
endif()
```

- [ ] **Step 4: Build and verify compilation (with and without OpenCV)**

With OpenCV:
```bash
cd "C:\共享文件夹\projects\smartcam\build"
cmake .. && cmake --build . -j$(nproc)
```
Expected: Compiles with `-DHAS_OPENCV`, links OpenCV.

Without OpenCV (simulate by disabling):
```bash
cmake .. -DENABLE_VIDEO_ENHANCE=OFF && cmake --build . -j$(nproc)
```
Expected: Compiles without HAS_OPENCV, VideoProcessor is a stub.

Re-enable after testing:
```bash
cmake .. -DENABLE_VIDEO_ENHANCE=ON
```

- [ ] **Step 5: Commit**

```bash
git add src/modules/video/video_processor.h src/modules/video/video_processor.cpp CMakeLists.txt
git commit -m "feat: add VideoProcessor module (CLAHE + NLMeans denoise with OpenCV)"
```

---

### Task 6: CameraCapture Enhancement Integration

**Files:**
- Modify: `src/modules/camera/camera_capture.h` — add `set_video_processor()`, member
- Modify: `src/modules/camera/camera_capture.cpp` — call VideoProcessor before OSD/encode

**Interfaces:**
- Consumes: `VideoProcessor` from Task 5
- Produces: `CameraCapture::set_video_processor(std::shared_ptr<VideoProcessor>) → void`

- [ ] **Step 1: Modify camera_capture.h**

Edit `src/modules/camera/camera_capture.h`. Add forward declaration and public method. After `set_actual_bitrate_callback()`, add:

```cpp
    // Inject optional video processor (CLAHE + denoise).
    // When set and enabled, frames are processed before OSD/encode.
    void set_video_processor(std::shared_ptr<class VideoProcessor> processor) {
        video_processor_ = std::move(processor);
    }
```

In the private section, after `std::atomic<bool> request_idr_{false};`, add:

```cpp
    // Enhanced video path (optional)
    std::shared_ptr<class VideoProcessor> video_processor_;
```

- [ ] **Step 2: Modify camera_capture.cpp**

Edit `src/modules/camera/camera_capture.cpp`. Add include at top:

```cpp
#include "modules/video/video_processor.h"
```

In `capture_loop()`, add the VideoProcessor call right before the OSD/encoder decision point. The relevant section currently reads:

```cpp
        if (osd_config_.enabled && filter_graph_) {
            if (av_buffersrc_add_frame_flags(buffersrc_ctx_, yuv_frame_,
                                              AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
```

Insert before this block:

```cpp
        // Apply video enhancement (CLAHE + denoise) before OSD overlay
        if (video_processor_ && video_processor_->is_enabled()) {
            video_processor_->process(
                yuv_frame_->data[0], yuv_frame_->data[1], yuv_frame_->data[2],
                static_cast<int>(width_), static_cast<int>(height_),
                yuv_frame_->linesize[0], yuv_frame_->linesize[1]);
        }

```

So the full section in capture_loop becomes:

```cpp
        // Apply video enhancement (CLAHE + denoise) before OSD overlay
        if (video_processor_ && video_processor_->is_enabled()) {
            video_processor_->process(
                yuv_frame_->data[0], yuv_frame_->data[1], yuv_frame_->data[2],
                static_cast<int>(width_), static_cast<int>(height_),
                yuv_frame_->linesize[0], yuv_frame_->linesize[1]);
        }

        if (osd_config_.enabled && filter_graph_) {
            if (av_buffersrc_add_frame_flags(buffersrc_ctx_, yuv_frame_,
                                              AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                SPDLOG_WARN("Failed to push frame to filter");
                continue;
            }
```

- [ ] **Step 3: Build and verify compilation**

Run:
```bash
cd "C:\共享文件夹\projects\smartcam\build"
cmake .. && cmake --build . -j$(nproc)
```

Expected: Compiles. VideoProcessor is not created yet (no one calls `set_video_processor()`), so the existing pipeline behavior is unchanged.

- [ ] **Step 4: Commit**

```bash
git add src/modules/camera/camera_capture.h src/modules/camera/camera_capture.cpp
git commit -m "feat: integrate VideoProcessor into CameraCapture pipeline"
```

---

### Task 7: MainService Wiring & End-to-End

**Files:**
- Modify: `src/app/main_service.h` — add processor shared_ptr members
- Modify: `src/app/main_service.cpp` — create processors, inject into capture modules, configure
- No new files, no test files needed (integration verified by running the binary)

**Interfaces:**
- Consumes: `AudioProcessor` from Task 3, `VideoProcessor` from Task 5
- Consumes: `AudioEnhanceConfig`, `VideoEnhanceConfig` from Task 2
- Produces: Full pipeline — AudioCapture and CameraCapture receive processors when config enables them

- [ ] **Step 1: Modify main_service.h**

Edit `src/app/main_service.h`. Add forward declarations and members. After the `#include` block, add:

```cpp
// Forward declare
namespace smartcam {
class AudioProcessor;
class VideoProcessor;
}
```

In the private section, after `std::unique_ptr<RtspServer> rtsp_;`, add:

```cpp
    // Enhancement processors (optional, config-driven)
    std::shared_ptr<AudioProcessor> audio_processor_;
    std::shared_ptr<VideoProcessor> video_processor_;
```

- [ ] **Step 2: Modify main_service.cpp — add includes and processor creation**

Edit `src/app/main_service.cpp`. Add includes at top:

```cpp
#include "modules/audio/audio_processor.h"
#include "modules/video/video_processor.h"
```

In `MainService::run()`, after `camera_->init()` and before `audio_->init()`, add processor creation. The current flow:

```cpp
    if (!camera_->init(config_.camera, config_.osd)) {
        SPDLOG_ERROR("Failed to initialize camera module");
        return;
    }

    if (config_.streaming.audio_enabled) {
        if (audio_->init(config_.streaming.audio_device)) {
```

Insert processor creation between these blocks:

```cpp
    if (!camera_->init(config_.camera, config_.osd)) {
        SPDLOG_ERROR("Failed to initialize camera module");
        return;
    }

    // Set up video enhancement processor
    if (config_.video_enhance.enabled) {
        video_processor_ = std::make_shared<VideoProcessor>();
        if (video_processor_->init(config_.video_enhance)) {
            camera_->set_video_processor(video_processor_);
            SPDLOG_INFO("Video enhancement processor injected into camera pipeline");
        } else {
            SPDLOG_WARN("Video enhancement init failed, continuing without");
            video_processor_.reset();
        }
    }

    // Set up audio enhancement processor
    if (config_.audio_enhance.enabled) {
        audio_processor_ = std::make_shared<AudioProcessor>();
        if (audio_processor_->init(config_.audio_enhance)) {
            audio_->set_audio_processor(audio_processor_);
            SPDLOG_INFO("Audio enhancement processor injected into audio pipeline");
        } else {
            SPDLOG_WARN("Audio enhancement init failed, continuing without");
            audio_processor_.reset();
        }
    }

    if (config_.streaming.audio_enabled) {
        if (audio_->init(config_.streaming.audio_device)) {
```

Note: `set_audio_processor()` must be called BEFORE `audio_->init()` because `init()` checks `audio_processor_->is_enabled()` to decide the sample rate.

- [ ] **Step 3: Build the complete binary**

Run:
```bash
cd "C:\共享文件夹\projects\smartcam\build"
cmake .. && cmake --build . -j$(nproc)
```

Expected: Full smartcam binary compiles and links with all enhancement modules wired.

- [ ] **Step 4: Verify backward compatibility (enhancement disabled)**

Create a test config `/tmp/smartcam_test_disabled.yaml`:
```yaml
camera:
  v4l2_device: /dev/video0
streaming:
  rtsp_port: 8554
  audio_enabled: false
audio_enhance:
  enabled: false
video_enhance:
  enabled: false
```

Run smartcam briefly to verify it starts without errors:
```bash
timeout 5 ./smartcam -c /tmp/smartcam_test_disabled.yaml 2>&1 || true
```

Expected: Normal startup messages, no enhancement-related logs (or "disabled" messages).

- [ ] **Step 5: Verify enhancement path initializes (with mocks)**

Create a test config `/tmp/smartcam_test_enabled.yaml`:
```yaml
camera:
  v4l2_device: /dev/video0
streaming:
  rtsp_port: 8554
  audio_enabled: true
  audio_device: plughw:0,0
audio_enhance:
  enabled: true
video_enhance:
  enabled: true
```

Run smartcam briefly:
```bash
timeout 5 ./smartcam -c /tmp/smartcam_test_enabled.yaml 2>&1 || true
```

Expected output should include:
- `AudioProcessor initialized: AGC(target=0.100, max_gain=30.0dB), RNNoise(level=1.0)`
- `VideoProcessor initialized: CLAHE(clip=2.0, tile=8), denoise(h=10.0, skip=2)`
- `AudioCapture: using 48kHz enhanced path`
- `Audio enhancement processor injected into audio pipeline`
- `Video enhancement processor injected into camera pipeline`

If video device is available, the pipeline should run normally with enhanced frames.

- [ ] **Step 6: Commit**

```bash
git add src/app/main_service.h src/app/main_service.cpp
git commit -m "feat: wire AudioProcessor and VideoProcessor into MainService with config-driven injection"
```

---

### Final Verification

- [ ] Build the complete project from scratch:

```bash
cd "C:\共享文件夹\projects\smartcam"
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

Expected: Clean build, zero warnings, smartcam binary produced.

- [ ] Verify the git log shows clean, incremental commits:

```bash
git log --oneline -7
```

Expected: 7 commits, one per task, in dependency order.
