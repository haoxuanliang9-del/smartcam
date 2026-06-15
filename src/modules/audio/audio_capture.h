#ifndef SMARTCAM_MODULES_AUDIO_CAPTURE_H
#define SMARTCAM_MODULES_AUDIO_CAPTURE_H

#include "common/config.h"
#include "common/types.h"
#include "middleware/message_queue.h"
#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>

struct _snd_pcm;

namespace smartcam {

class AudioCapture {
public:
    AudioCapture() = default;
    ~AudioCapture();

    bool init(const std::string& device = "plughw:0,0");
    void start();
    void stop();

    void add_client_queue(std::shared_ptr<MessageQueue<std::shared_ptr<AudioFrame>>> queue);
    void remove_client_queue(std::shared_ptr<MessageQueue<std::shared_ptr<AudioFrame>>> queue);

    bool is_running() const { return running_; }

private:
    void capture_loop();
    uint8_t pcmu_encode(int16_t sample);

    std::vector<std::shared_ptr<MessageQueue<std::shared_ptr<AudioFrame>>>> client_queues_;
    std::mutex queues_mutex_;

    _snd_pcm* pcm_ = nullptr;
    std::string device_;
    std::atomic<bool> running_{false};
    std::thread capture_thread_;
};

} // namespace smartcam

#endif // SMARTCAM_MODULES_AUDIO_CAPTURE_H
