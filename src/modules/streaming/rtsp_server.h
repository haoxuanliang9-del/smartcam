#ifndef SMARTCAM_MODULES_RTSP_SERVER_H
#define SMARTCAM_MODULES_RTSP_SERVER_H

#include "common/config.h"
#include "common/types.h"
#include "middleware/message_queue.h"
#include <memory>
#include <string>
#include <atomic>

namespace smartcam {

class RtspServer {
public:
    explicit RtspServer(std::shared_ptr<MessageQueue<Frame>> frame_queue);
    ~RtspServer();

    bool init(const StreamingConfig& config, uint32_t width, uint32_t height, uint32_t fps);
    void start();
    void stop();

    std::string get_url() const;
    bool is_running() const { return running_; }

private:
    void server_loop();

    std::shared_ptr<MessageQueue<Frame>> frame_queue_;
    StreamingConfig config_;
    uint32_t width_ = 1920;
    uint32_t height_ = 1080;
    uint32_t fps_ = 30;

    std::atomic<bool> running_{false};
    void* live_server_ = nullptr;
    void* live_session_ = nullptr;
};

} // namespace smartcam

#endif // SMARTCAM_MODULES_RTSP_SERVER_H
