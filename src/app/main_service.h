#ifndef SMARTCAM_APP_MAIN_SERVICE_H
#define SMARTCAM_APP_MAIN_SERVICE_H

#include "common/config.h"
#include "middleware/message_queue.h"
#include "middleware/timer_manager.h"
#include "modules/camera/camera_capture.h"
#include "modules/sensor/sensor_module.h"
#include "modules/analysis/intelligent_analysis.h"
#include "modules/streaming/rtsp_server.h"
#include "modules/ota/ota_manager.h"
#include <asio.hpp>
#include <memory>
#include <atomic>

namespace smartcam {

class MainService {
public:
    MainService();
    ~MainService();

    bool init(const std::string& config_path);
    void run();
    void shutdown();

private:
    void setup_logging();
    void setup_modules();
    void connect_callbacks();

    asio::io_context io_ctx_;
    TimerManager timer_manager_;

    Config config_;

    std::shared_ptr<MessageQueue<Frame>> frame_queue_;
    std::shared_ptr<MessageQueue<SensorData>> sensor_queue_;

    std::unique_ptr<CameraCapture> camera_;
    std::unique_ptr<SensorModule> sensor_;
    std::unique_ptr<IntelligentAnalysis> analysis_;
    std::unique_ptr<RtspServer> rtsp_;
    std::unique_ptr<OtaManager> ota_;

    std::atomic<bool> running_{false};
};

} // namespace smartcam

#endif // SMARTCAM_APP_MAIN_SERVICE_H
