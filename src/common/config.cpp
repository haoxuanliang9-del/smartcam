#include "config.h"
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

namespace smartcam {

bool Config::load(const std::string& path) {
    try {
        YAML::Node root = YAML::LoadFile(path);

        if (root["system"]) {
            auto n = root["system"];
            if (n["device_id"]) system.device_id = n["device_id"].as<std::string>();
            if (n["log_level"]) system.log_level = n["log_level"].as<std::string>();
            if (n["log_file"]) system.log_file = n["log_file"].as<std::string>();
            if (n["log_max_size_mb"]) system.log_max_size_mb = n["log_max_size_mb"].as<uint32_t>();
            if (n["log_max_files"]) system.log_max_files = n["log_max_files"].as<uint32_t>();
        }

        if (root["camera"]) {
            auto n = root["camera"];
            if (n["bitrate_kbps"]) camera.bitrate_kbps = n["bitrate_kbps"].as<uint32_t>();
            if (n["v4l2_device"]) camera.v4l2_device = n["v4l2_device"].as<std::string>();
            if (n["v4l2_width"]) camera.v4l2_width = n["v4l2_width"].as<uint32_t>();
            if (n["v4l2_height"]) camera.v4l2_height = n["v4l2_height"].as<uint32_t>();
            if (n["v4l2_fps"]) camera.v4l2_fps = n["v4l2_fps"].as<uint32_t>();
            if (n["v4l2_pix_fmt"]) camera.v4l2_pix_fmt = n["v4l2_pix_fmt"].as<std::string>();
        }

        if (root["sensor"]) {
            auto n = root["sensor"];
            if (n["i2c_bus"]) sensor.i2c_bus = n["i2c_bus"].as<int>();
            if (n["i2c_addr"]) sensor.i2c_addr = n["i2c_addr"].as<uint8_t>();
            if (n["sample_interval_sec"]) sensor.sample_interval_sec = n["sample_interval_sec"].as<uint32_t>();
        }

        if (root["streaming"]) {
            auto n = root["streaming"];
            if (n["rtsp_port"]) streaming.rtsp_port = n["rtsp_port"].as<uint16_t>();
            if (n["stream_name"]) streaming.stream_name = n["stream_name"].as<std::string>();
            if (n["max_clients"]) streaming.max_clients = n["max_clients"].as<uint32_t>();
            if (n["audio_device"]) streaming.audio_device = n["audio_device"].as<std::string>();
            if (n["audio_enabled"]) streaming.audio_enabled = n["audio_enabled"].as<bool>();
            if (n["audio_volume"]) streaming.audio_volume = n["audio_volume"].as<float>();
        }

        if (root["osd"]) {
            auto n = root["osd"];
            if (n["enabled"]) osd.enabled = n["enabled"].as<bool>();
            if (n["font_size"]) osd.font_size = n["font_size"].as<uint32_t>();
        }

        if (root["display"]) {
            auto n = root["display"];
            if (n["enabled"]) display.enabled = n["enabled"].as<bool>();
            if (n["spi_device"]) display.spi_device = n["spi_device"].as<std::string>();
            if (n["gpio_dc"]) display.gpio_dc = n["gpio_dc"].as<int>();
            if (n["gpio_rst"]) display.gpio_rst = n["gpio_rst"].as<int>();
            if (n["gpio_cs"]) display.gpio_cs = n["gpio_cs"].as<int>();
            if (n["refresh_sec"]) display.refresh_sec = n["refresh_sec"].as<uint32_t>();
        }

        if (root["audio_enhance"]) {
            auto n = root["audio_enhance"];
            if (n["enabled"]) audio_enhance.enabled = n["enabled"].as<bool>();
            if (n["agc_target_rms"]) audio_enhance.agc_target_rms = n["agc_target_rms"].as<float>();
            if (n["agc_max_gain"]) audio_enhance.agc_max_gain = n["agc_max_gain"].as<float>();
            if (n["agc_attack_ms"]) audio_enhance.agc_attack_ms = n["agc_attack_ms"].as<float>();
            if (n["agc_release_ms"]) audio_enhance.agc_release_ms = n["agc_release_ms"].as<float>();
            if (n["denoise_level"]) audio_enhance.denoise_level = n["denoise_level"].as<float>();
            if (n["rnnoise_model"]) audio_enhance.rnnoise_model = n["rnnoise_model"].as<std::string>();
        }

        if (root["video_enhance"]) {
            auto n = root["video_enhance"];
            if (n["enabled"]) video_enhance.enabled = n["enabled"].as<bool>();
            if (n["clahe_clip_limit"]) video_enhance.clahe_clip_limit = n["clahe_clip_limit"].as<float>();
            if (n["clahe_tile_size"]) video_enhance.clahe_tile_size = n["clahe_tile_size"].as<int>();
            if (n["denoise_h"]) video_enhance.denoise_h = n["denoise_h"].as<float>();
            if (n["denoise_skip_frames"]) video_enhance.denoise_skip_frames = n["denoise_skip_frames"].as<int>();
        }

        SPDLOG_INFO("Configuration loaded from {}", path);
        return true;

    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to load config: {}", e.what());
        return false;
    }
}

} // namespace smartcam
