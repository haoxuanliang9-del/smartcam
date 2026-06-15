#ifndef SMARTCAM_APP_MAIN_SERVICE_H
#define SMARTCAM_APP_MAIN_SERVICE_H

#include "common/config.h"
#include "middleware/message_queue.h"
#include "modules/camera/camera_capture.h"
#include "modules/audio/audio_capture.h"
#include "modules/sensor/sensor_module.h"
#include "modules/display/oled_display.h"
#include "modules/streaming/rtsp_server.h"
#include <memory>
#include <atomic>

namespace smartcam {

class MainService {
public:
    MainService() = default;
    ~MainService();

    bool init(const std::string& config_path);
    void run();
    void shutdown();

private:
    void setup_logging();
    void setup_modules();
    void connect_callbacks();

    Config config_;
    std::shared_ptr<MessageQueue<SensorData>> sensor_queue_ = std::make_shared<MessageQueue<SensorData>>(64);

    std::shared_ptr<CameraCapture> camera_;
    std::shared_ptr<AudioCapture> audio_;
    std::unique_ptr<SensorModule> sensor_;
    std::unique_ptr<OledDisplay> display_;
    std::unique_ptr<RtspServer> rtsp_;

    std::atomic<bool> running_{false};
    std::atomic<uint32_t> latest_bitrate_kbps_{0};
};

} // namespace smartcam

#endif // SMARTCAM_APP_MAIN_SERVICE_H
