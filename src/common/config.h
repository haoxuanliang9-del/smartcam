#ifndef SMARTCAM_COMMON_CONFIG_H
#define SMARTCAM_COMMON_CONFIG_H

#include <string>
#include <cstdint>

namespace smartcam {

struct SystemConfig {
    std::string device_id;
    std::string log_level = "info";
    std::string log_file;
    uint32_t log_max_size_mb = 5;
    uint32_t log_max_files = 3;
};

struct CameraConfig {
    std::string video_file = "/root/video.mp4";
    uint32_t bitrate_kbps = 1000;
};

struct SensorConfig {
    int i2c_bus = 1;
    uint8_t i2c_addr = 0x38;
    uint32_t sample_interval_sec = 1;
};

struct StreamingConfig {
    uint16_t rtsp_port = 8554;
    std::string stream_name = "live";
    uint32_t max_clients = 10;
    std::string audio_device = "plughw:0,0";
};

struct OsdConfig {
    bool enabled = true;
    uint32_t font_size = 24;
};

struct DisplayConfig {
    bool enabled = false;
    std::string spi_device = "/dev/spidev0.0";
    int gpio_dc = 14;
    int gpio_rst = 9;
    int gpio_cs = 10;
    uint32_t refresh_sec = 1;
};

struct Config {
    SystemConfig system;
    CameraConfig camera;
    SensorConfig sensor;
    StreamingConfig streaming;
    OsdConfig osd;
    DisplayConfig display;

    bool load(const std::string& path);
};

} // namespace smartcam

#endif // SMARTCAM_COMMON_CONFIG_H
