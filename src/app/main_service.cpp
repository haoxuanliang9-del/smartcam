#include "main_service.h"
#include "modules/audio/audio_processor.h"
#include "modules/video/video_processor.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <csignal>
#include <cstdio>
#include <thread>

namespace smartcam {

static std::atomic<bool> g_shutdown_requested{false};

static void signal_handler(int signum) {
    SPDLOG_INFO("Received signal {}, shutting down...", signum);
    g_shutdown_requested = true;
}

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
    camera_ = std::make_shared<CameraCapture>();
    audio_ = std::make_shared<AudioCapture>();
    sensor_ = std::make_unique<SensorModule>();
    rtsp_ = std::make_unique<RtspServer>();
    rtsp_->set_camera(camera_);
    rtsp_->set_audio(audio_);
    if (config_.display.enabled) {
        display_ = std::make_unique<OledDisplay>();
    }
}

void MainService::connect_callbacks() {
    camera_->set_actual_bitrate_callback([this](uint32_t kbps) {
        if (display_) display_->update_bitrate(kbps);
        latest_bitrate_kbps_ = kbps;
    });

    if (sensor_) {
        sensor_->set_data_callback([this](float t, float h) {
            if (display_) display_->update_sensor(t, h);
            char buf[128];
            snprintf(buf, sizeof(buf), "Temp: %.1fC  Hum: %.1f%%  |  %u kbps",
                     t, h, latest_bitrate_kbps_.load());
            camera_->update_osd_text(buf);
        });
    }
}

void MainService::run() {
    running_ = true;

    if (!camera_->init(config_.camera, config_.osd)) {
        SPDLOG_ERROR("Failed to initialize camera module");
        return;
    }

    // Set up video enhancement processor
    video_processor_ = std::make_shared<VideoProcessor>();
    if (video_processor_->init(config_.video_enhance)) {
        camera_->set_video_processor(video_processor_);
        SPDLOG_INFO("Video enhancement processor injected into camera pipeline");
    } else {
        SPDLOG_ERROR("Video enhancement init failed");
        video_processor_.reset();
    }

    // Set up audio enhancement processor
    audio_processor_ = std::make_shared<AudioProcessor>();
    if (audio_processor_->init(config_.audio_enhance)) {
        audio_->set_audio_processor(audio_processor_);
        SPDLOG_INFO("Audio enhancement processor injected into audio pipeline");
    } else {
        SPDLOG_ERROR("Audio enhancement init failed");
        audio_processor_.reset();
    }

    if (config_.streaming.audio_enabled) {
        if (audio_->init(config_.streaming.audio_device)) {
            audio_->set_volume(config_.streaming.audio_volume);
            SPDLOG_INFO("Audio volume gain: {:.1f}x", config_.streaming.audio_volume);
        } else {
            SPDLOG_WARN("Failed to initialize audio capture, continuing without audio");
            audio_.reset();
        }
    } else {
        SPDLOG_INFO("Audio disabled by config");
        audio_.reset();
    }

    bool sensor_init_ok = sensor_->init(config_.sensor);
    if (!sensor_init_ok) {
        SPDLOG_WARN("Failed to initialize sensor module, continuing without sensor");
        sensor_.reset();
    }

    if (display_ && !display_->init(config_.display)) {
        SPDLOG_WARN("Failed to initialize OLED display, continuing without display");
        display_.reset();
    }

    if (!rtsp_->init(config_.streaming)) {
        SPDLOG_ERROR("Failed to initialize RTSP server");
        return;
    }

    camera_->start();
    if (audio_) audio_->start();
    if (sensor_init_ok) sensor_->start();
    if (display_) display_->start();
    rtsp_->start();

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

    if (rtsp_) rtsp_->stop();
    if (audio_) audio_->stop();
    if (sensor_) sensor_->stop();
    if (display_) display_->stop();
    if (camera_) camera_->stop();

    // Release enhancement processors while TLS is still valid.
    // OpenCV's CLAHE Ptr accesses thread-local storage in its destructor;
    // resetting here avoids a TLS-use-after-destroy crash during global cleanup.
    video_processor_.reset();
    audio_processor_.reset();

    SPDLOG_INFO("SmartCam service stopped");
}

} // namespace smartcam
