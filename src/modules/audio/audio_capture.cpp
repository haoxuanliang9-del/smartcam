#include "audio_capture.h"
#include <spdlog/spdlog.h>
#include <alsa/asoundlib.h>
#include <cstring>
#include <chrono>
#include <algorithm>

namespace smartcam {

// ITU-T G.711 μ-law 编码查表 (13段折线, 8位编码)
// 将 14-bit 有符号线性值编码为 8-bit μ-law
static const uint8_t ulaw_table[256] = {
    0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
    5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
};

uint8_t AudioCapture::pcmu_encode(int16_t sample) {
    // μ-law 编码算法 (RFC 3551 / ITU-T G.711)
    int sign = (sample >> 8) & 0x80;
    if (sign) sample = -sample;
    if (sample > 32635) sample = 32635;

    // 将 14-bit 线性值转换为 8-bit μ-law
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

    unsigned int rate = 8000;
    snd_pcm_hw_params_set_rate_near(pcm_, hw_params, &rate, nullptr);

    snd_pcm_uframes_t period_size = 160; // 20ms @ 8kHz
    snd_pcm_hw_params_set_period_size_near(pcm_, hw_params, &period_size, nullptr);

    snd_pcm_uframes_t buffer_size = period_size * 4;
    snd_pcm_hw_params_set_buffer_size_near(pcm_, hw_params, &buffer_size);

    err = snd_pcm_hw_params(pcm_, hw_params);
    if (err < 0) {
        SPDLOG_ERROR("AudioCapture: cannot set hw params: {}", snd_strerror(err));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }

    snd_pcm_prepare(pcm_);

    SPDLOG_INFO("AudioCapture initialized: device={}, rate=8000, channels=1, period=160", device_);
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
    const int period_frames = 160; // 20ms @ 8kHz
    std::vector<int16_t> pcm_buf(period_frames);
    std::vector<uint8_t> ulaw_buf(period_frames);

    // 关键修复：基于"累计采样数"推算时间戳，而不是 wall clock
    // 因为 ALSA 会预填内部缓冲，连续两次 snd_pcm_readi 之间 wall clock
    // 可能只增加 66us，但 samples 增加了 320，会导致 RTP ts 重复 / 倒退
    uint64_t total_samples = 0;
    const uint32_t sample_rate = 8000;

    while (running_) {
        snd_pcm_sframes_t frames = snd_pcm_readi(pcm_, pcm_buf.data(), period_frames);

        if (frames < 0) {
            // ALSA 缓冲区欠载，恢复
            frames = snd_pcm_recover(pcm_, frames, 0);
            if (frames < 0) {
                SPDLOG_ERROR("AudioCapture: ALSA read failed: {}", snd_strerror(frames));
                break;
            }
            continue;
        }

        if (frames == 0) continue;

        // 软件增益（VM 上常见 mic 信号很弱，放大后才听得清）
        float vol = volume_.load();
        if (vol != 1.0f) {
            for (snd_pcm_sframes_t i = 0; i < frames; i++) {
                int32_t s = static_cast<int32_t>(pcm_buf[i] * vol);
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                pcm_buf[i] = static_cast<int16_t>(s);
            }
        }

        // 噪声门：低幅值低于阈值（默认 1500 ≈ -27dBFS）视为环境噪声，强制静默
        // 解决 VMware 虚拟声卡 + 笔记本 mic 的底噪问题
        const int16_t noise_gate = 1500;
        int16_t peak = 0;
        for (snd_pcm_sframes_t i = 0; i < frames; i++) {
            int16_t a = pcm_buf[i] >= 0 ? pcm_buf[i] : -pcm_buf[i];
            if (a > peak) peak = a;
        }
        if (peak < noise_gate) {
            std::fill(pcm_buf.begin(), pcm_buf.begin() + frames, 0);
        }

        // G.711 PCMU 编码
        for (snd_pcm_sframes_t i = 0; i < frames; i++) {
            ulaw_buf[i] = pcmu_encode(pcm_buf[i]);
        }

        // 计算时间戳（微秒）：基于累计采样数推算，保证单调递增
        uint64_t timestamp = (total_samples * 1000000ULL) / sample_rate;
        total_samples += static_cast<uint64_t>(frames);

        AudioFrame frame;
        frame.timestamp = timestamp;
        frame.data.assign(ulaw_buf.begin(), ulaw_buf.begin() + frames);

        // 使用 shared_ptr 分发音频帧，避免多客户端拷贝
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
