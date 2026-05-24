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
            if (n["video_file"]) camera.video_file = n["video_file"].as<std::string>();
            if (n["bitrate_kbps"]) camera.bitrate_kbps = n["bitrate_kbps"].as<uint32_t>();
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

        SPDLOG_INFO("Configuration loaded from {}", path);
        return true;

    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to load config: {}", e.what());
        return false;
    }
}

} // namespace smartcam
