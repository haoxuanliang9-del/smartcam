#include "audio_capture.h"
#include "audio_processor.h"
#include <spdlog/spdlog.h>
#include <alsa/asoundlib.h>
#include <cstring>
#include <chrono>
#include <algorithm>

namespace smartcam {

// ITU-T G.711 μ-law encoding
uint8_t AudioCapture::pcmu_encode(int16_t sample) {
    int sign = (sample >> 8) & 0x80;
    // Guard INT16_MIN (-32768) against two's-complement negation overflow
    if (sign) sample = (sample == -32768) ? 32767 : -sample;
    if (sample > 32635) sample = 32635;

    int exponent = 7;
    for (int exp = 7; exp > 0; exp--) {
        if (sample & (1 << (exp + 6))) {
            exponent = exp;
            break;
        }
    }
    int mantissa = (sample >> (exponent + 3)) & 0x0F;
    uint8_t ulaw = ~(sign | (exponent << 4) | mantissa);
    return ulaw;
}

AudioCapture::~AudioCapture() {
    stop();
}

bool AudioCapture::init(const std::string& device) {
    device_ = device;

    // Always 48kHz — AudioProcessor handles downsampling to 8kHz for PCMU
    unsigned int rate = 48000;
    snd_pcm_uframes_t period_size = 480; // 10ms @ 48kHz

    int err = snd_pcm_open(&pcm_, device_.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        SPDLOG_ERROR("AudioCapture: cannot open ALSA device '{}': {}", device_, snd_strerror(err));
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
        return false;
    }

    snd_pcm_prepare(pcm_);

    SPDLOG_INFO("AudioCapture initialized: device={}, rate=48000, channels=1, period={}",
                device_, period_near);
    return true;
}

void AudioCapture::start() {
    if (running_) return;
    running_ = true;
    capture_thread_ = std::thread(&AudioCapture::capture_loop, this);
    SPDLOG_INFO("AudioCapture started");
}

void AudioCapture::stop() {
    running_ = false;
    if (capture_thread_.joinable()) capture_thread_.join();
    if (pcm_) {
        snd_pcm_drop(pcm_);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
    SPDLOG_INFO("AudioCapture stopped");
}

void AudioCapture::capture_loop() {
    // 48kHz capture → AudioProcessor (AGC+RNNoise+resample) → 8kHz → noise gate → PCMU
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

        // Apply user-configured software gain
        float vol = volume_.load();
        if (vol != 1.0f) {
            for (size_t i = 0; i < out_samples; i++) {
                int32_t s = static_cast<int32_t>(pcm_8k[i] * vol);
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                pcm_8k[i] = static_cast<int16_t>(s);
            }
        }

        // Noise gate with VAD assist (gate only non-speech silence).
        // Low threshold since RNNoise handles noise suppression;
        // VAD prevents gating speech frames that RNNoise partially suppressed.
        const int16_t noise_gate = 30;
        float vad = audio_processor_->last_vad();
        int16_t peak = 0;
        for (size_t i = 0; i < out_samples; i++) {
            int16_t a = pcm_8k[i] >= 0 ? pcm_8k[i] : -pcm_8k[i];
            if (a > peak) peak = a;
        }
        SPDLOG_DEBUG("Audio noise gate: peak={}, vad={:.3f}, gate={}",
                     peak, vad, (peak < noise_gate && vad < 0.3f) ? "ON" : "OFF");
        if (peak < noise_gate && vad < 0.3f) {
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
}

void AudioCapture::add_client_queue(std::shared_ptr<LatestValue<AudioFrame>> slot) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    client_slots_.push_back(slot);
    SPDLOG_INFO("Audio client slot added, total: {}", client_slots_.size());
}

void AudioCapture::remove_client_queue(std::shared_ptr<LatestValue<AudioFrame>> slot) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = std::remove(client_slots_.begin(), client_slots_.end(), slot);
    if (it != client_slots_.end()) {
        client_slots_.erase(it, client_slots_.end());
        SPDLOG_INFO("Audio client slot removed, remaining: {}", client_slots_.size());
    }
}

} // namespace smartcam
