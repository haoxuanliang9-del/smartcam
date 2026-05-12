#include "main_service.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sink.h>
#include <csignal>

namespace smartcam {

static std::atomic<bool> g_shutdown_requested{false};

static void signal_handler(int signum) {
    SPDLOG_INFO("Received signal {}, shutting down...", signum);
    g_shutdown_requested = true;
}

MainService::MainService()
    : timer_manager_(io_ctx_)
    , frame_queue_(std::make_shared<MessageQueue<Frame>>(64))
    , sensor_queue_(std::make_shared<MessageQueue<SensorData>>(64)) {}

MainService::~MainService() {
    shutdown();
}

bool MainService::init(const std::string& config_path) {
    if (!config_.load(config_path)) {
        SPDLOG_WARN("Failed to load config from {}, using defaults", config_path);
    }

    setup_logging();
    setup_modules();
    connect_callbacks();

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    SPDLOG_INFO("SmartCam service initialized");
    return true;
}

void MainService::setup_logging() {
    std::vector<spdlog::sink_ptr> sinks;

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::debug);
    sinks.push_back(console_sink);

    if (!config_.system.log_file.empty()) {
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            config_.system.log_file,
            config_.system.log_max_size_mb * 1024 * 1024,
            config_.system.log_max_files);
        file_sink->set_level(spdlog::level::info);
        sinks.push_back(file_sink);
    }

    auto logger = std::make_shared<spdlog::logger>("smartcam", sinks.begin(), sinks.end());

    std::string level = config_.system.log_level;
    if (level == "debug") logger->set_level(spdlog::level::debug);
    else if (level == "warn") logger->set_level(spdlog::level::warn);
    else if (level == "error") logger->set_level(spdlog::level::err);
    else logger->set_level(spdlog::level::info);

    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
}

void MainService::setup_modules() {
    camera_ = std::make_unique<CameraCapture>(frame_queue_);
    sensor_ = std::make_unique<SensorModule>(sensor_queue_);
    analysis_ = std::make_unique<IntelligentAnalysis>(sensor_queue_, frame_queue_);
    rtsp_ = std::make_unique<RtspServer>(frame_queue_);
    ota_ = std::make_unique<OtaManager>();
}

void MainService::connect_callbacks() {
    analysis_->set_bitrate_callback([this](uint32_t bitrate) {
        if (camera_) camera_->set_bitrate(bitrate);
    });

    analysis_->set_osd_callback([this](const std::string& text) {
        if (camera_) camera_->update_osd_text(text);
    });
}

void MainService::run() {
    running_ = true;

    if (!camera_->init(config_.camera, config_.osd)) {
        SPDLOG_ERROR("Failed to initialize camera module");
        return;
    }

    if (!sensor_->init(config_.sensor)) {
        SPDLOG_WARN("Failed to initialize sensor module, continuing without sensor");
    }

    if (!analysis_->init(config_.analysis, config_.camera.bitrate_kbps)) {
        SPDLOG_ERROR("Failed to initialize analysis module");
        return;
    }

    if (!rtsp_->init(config_.streaming, config_.camera.width,
                     config_.camera.height, config_.camera.fps)) {
        SPDLOG_ERROR("Failed to initialize RTSP server");
        return;
    }

    if (!config_.ota.server_url.empty()) {
        if (!ota_->init(config_.ota)) {
            SPDLOG_WARN("Failed to initialize OTA manager");
        }
    }

    camera_->start();
    sensor_->start();
    analysis_->start();
    rtsp_->start();
    ota_->start();

    SPDLOG_INFO("SmartCam service running (RTSP: {})", rtsp_->get_url());

    while (!g_shutdown_requested && running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    shutdown();
}

void MainService::shutdown() {
    if (!running_) return;
    running_ = false;

    SPDLOG_INFO("Shutting down SmartCam service...");

    if (ota_) ota_->stop();
    if (rtsp_) rtsp_->stop();
    if (analysis_) analysis_->stop();
    if (sensor_) sensor_->stop();
    if (camera_) camera_->stop();

    timer_manager_.cancel_all();

    SPDLOG_INFO("SmartCam service stopped");
}

} // namespace smartcam
