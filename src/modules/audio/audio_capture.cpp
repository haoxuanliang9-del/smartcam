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

    auto start_time = std::chrono::steady_clock::now();

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

        // G.711 PCMU 编码
        for (snd_pcm_sframes_t i = 0; i < frames; i++) {
            ulaw_buf[i] = pcmu_encode(pcm_buf[i]);
        }

        // 计算时间戳（微秒）
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start_time);
        uint64_t timestamp = static_cast<uint64_t>(elapsed.count());

        AudioFrame frame;
        frame.timestamp = timestamp;
        frame.data.assign(ulaw_buf.begin(), ulaw_buf.begin() + frames);

        // 使用 shared_ptr 分发音频帧，避免多客户端拷贝
        auto shared_frame = std::make_shared<AudioFrame>(std::move(frame));

        {
            std::lock_guard<std::mutex> lock(queues_mutex_);
            for (auto& queue : client_queues_) {
                if (!queue->push(shared_frame)) {
                    SPDLOG_DEBUG("Audio frame dropped for client queue, queue full");
                }
            }
        }
    }
}

void AudioCapture::add_client_queue(std::shared_ptr<MessageQueue<std::shared_ptr<AudioFrame>>> queue) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    client_queues_.push_back(queue);
    SPDLOG_INFO("Audio client queue added, total queues: {}", client_queues_.size());
}

void AudioCapture::remove_client_queue(std::shared_ptr<MessageQueue<std::shared_ptr<AudioFrame>>> queue) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = std::remove(client_queues_.begin(), client_queues_.end(), queue);
    if (it != client_queues_.end()) {
        client_queues_.erase(it, client_queues_.end());
        SPDLOG_INFO("Audio client queue removed, remaining queues: {}", client_queues_.size());
    }
}

} // namespace smartcam
