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
    // V4L2 camera configuration
    std::string v4l2_device = "/dev/video0";
    uint32_t v4l2_width = 1280;
    uint32_t v4l2_height = 720;
    uint32_t v4l2_fps = 10;
    std::string v4l2_pix_fmt = "yuyv422";
    uint32_t bitrate_kbps = 2500;
};

struct SensorConfig {
    int i2c_bus = 1;
    uint8_t i2c_addr = 0x38;
    uint32_t sample_interval_sec = 1;
};

struct StreamingConfig {
    uint16_t rtsp_port = 8554;
    std::string stream_name = "live";
    std::string audio_device = "plughw:0,0";
    bool audio_enabled = true;
    float audio_volume = 1.0f;
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

struct AudioEnhanceConfig {
    bool enabled = false;
    float agc_target_rms = 0.1f;       // -20dBFS
    float agc_max_gain = 30.0f;        // +30dB max
    float agc_attack_ms = 5.0f;
    float agc_release_ms = 100.0f;
    float denoise_level = 1.0f;        // 0.0=bypass, 1.0=max
    std::string rnnoise_model;         // empty = built-in model
};

struct VideoEnhanceConfig {
    bool enabled = false;
    float clahe_clip_limit = 2.0f;
    int clahe_tile_size = 8;
    float denoise_h = 10.0f;           // 0 = bypass denoise
    int denoise_skip_frames = 2;       // process every Nth frame
};

struct Config {
    SystemConfig system;
    CameraConfig camera;
    SensorConfig sensor;
    StreamingConfig streaming;
    OsdConfig osd;
    DisplayConfig display;
    AudioEnhanceConfig audio_enhance;
    VideoEnhanceConfig video_enhance;

    bool load(const std::string& path);
};

} // namespace smartcam

#endif // SMARTCAM_COMMON_CONFIG_H
