#ifndef SMARTCAM_COMMON_CONFIG_H
#define SMARTCAM_COMMON_CONFIG_H

#include <string>
#include <cstdint>

namespace smartcam {

struct SystemConfig {
    std::string device_id;
    std::string log_level;
    std::string log_file;
    uint32_t log_max_size_mb = 5;
    uint32_t log_max_files = 3;
};

struct CameraConfig {
    std::string device = "/dev/video0";
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 30;
    std::string input_format = "YUYV";
    uint32_t bitrate_kbps = 2000;
};

struct SensorConfig {
    int i2c_bus = 1;
    uint8_t i2c_addr = 0x38;
    uint32_t sample_interval_sec = 1;
    float temp_threshold = 2.0f;
    float humidity_threshold = 5.0f;
};

struct StreamingConfig {
    uint16_t rtsp_port = 8554;
    std::string stream_name = "live";
    uint32_t max_clients = 10;
};

struct MotionDetectionConfig {
    bool enabled = true;
    float sensitivity = 0.3f;
    int min_area = 500;
    uint32_t check_interval_ms = 200;
};

struct BitrateAdaptationConfig {
    bool enabled = true;
    float boost_factor = 1.5f;
    uint32_t recovery_time_sec = 30;
};

struct AnalysisConfig {
    MotionDetectionConfig motion_detection;
    BitrateAdaptationConfig bitrate_adaptation;
};

struct OsdConfig {
    bool enabled = true;
    std::string position = "top-right";
    uint32_t font_size = 24;
};

struct OtaConfig {
    std::string server_url;
    uint32_t check_interval_sec = 3600;
    std::string certificate;
    std::string current_version = "1.0.0";
};

struct Config {
    SystemConfig system;
    CameraConfig camera;
    SensorConfig sensor;
    StreamingConfig streaming;
    AnalysisConfig analysis;
    OsdConfig osd;
    OtaConfig ota;

    bool load(const std::string& path);
    bool save(const std::string& path);
};

} // namespace smartcam

#endif // SMARTCAM_COMMON_CONFIG_H
