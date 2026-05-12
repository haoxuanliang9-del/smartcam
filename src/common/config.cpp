#include "config.h"
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

namespace smartcam {

bool Config::load(const std::string& path) {
    try {
        YAML::Node root = YAML::LoadFile(path);

        if (root["system"]) {
            auto& n = root["system"];
            if (n["device_id"]) system.device_id = n["device_id"].as<std::string>();
            if (n["log_level"]) system.log_level = n["log_level"].as<std::string>();
            if (n["log_file"]) system.log_file = n["log_file"].as<std::string>();
            if (n["log_max_size_mb"]) system.log_max_size_mb = n["log_max_size_mb"].as<uint32_t>();
            if (n["log_max_files"]) system.log_max_files = n["log_max_files"].as<uint32_t>();
        }

        if (root["camera"]) {
            auto& n = root["camera"];
            if (n["device"]) camera.device = n["device"].as<std::string>();
            if (n["width"]) camera.width = n["width"].as<uint32_t>();
            if (n["height"]) camera.height = n["height"].as<uint32_t>();
            if (n["fps"]) camera.fps = n["fps"].as<uint32_t>();
            if (n["input_format"]) camera.input_format = n["input_format"].as<std::string>();
            if (n["bitrate_kbps"]) camera.bitrate_kbps = n["bitrate_kbps"].as<uint32_t>();
        }

        if (root["sensor"]) {
            auto& n = root["sensor"];
            if (n["i2c_bus"]) sensor.i2c_bus = n["i2c_bus"].as<int>();
            if (n["i2c_addr"]) sensor.i2c_addr = n["i2c_addr"].as<uint8_t>();
            if (n["sample_interval_sec"]) sensor.sample_interval_sec = n["sample_interval_sec"].as<uint32_t>();
            if (n["temp_threshold"]) sensor.temp_threshold = n["temp_threshold"].as<float>();
            if (n["humidity_threshold"]) sensor.humidity_threshold = n["humidity_threshold"].as<float>();
        }

        if (root["streaming"]) {
            auto& n = root["streaming"];
            if (n["rtsp_port"]) streaming.rtsp_port = n["rtsp_port"].as<uint16_t>();
            if (n["stream_name"]) streaming.stream_name = n["stream_name"].as<std::string>();
            if (n["max_clients"]) streaming.max_clients = n["max_clients"].as<uint32_t>();
        }

        if (root["analysis"]) {
            if (root["analysis"]["motion_detection"]) {
                auto& n = root["analysis"]["motion_detection"];
                if (n["enabled"]) analysis.motion_detection.enabled = n["enabled"].as<bool>();
                if (n["sensitivity"]) analysis.motion_detection.sensitivity = n["sensitivity"].as<float>();
                if (n["min_area"]) analysis.motion_detection.min_area = n["min_area"].as<int>();
                if (n["check_interval_ms"]) analysis.motion_detection.check_interval_ms = n["check_interval_ms"].as<uint32_t>();
            }
            if (root["analysis"]["bitrate_adaptation"]) {
                auto& n = root["analysis"]["bitrate_adaptation"];
                if (n["enabled"]) analysis.bitrate_adaptation.enabled = n["enabled"].as<bool>();
                if (n["boost_factor"]) analysis.bitrate_adaptation.boost_factor = n["boost_factor"].as<float>();
                if (n["recovery_time_sec"]) analysis.bitrate_adaptation.recovery_time_sec = n["recovery_time_sec"].as<uint32_t>();
            }
        }

        if (root["osd"]) {
            auto& n = root["osd"];
            if (n["enabled"]) osd.enabled = n["enabled"].as<bool>();
            if (n["position"]) osd.position = n["position"].as<std::string>();
            if (n["font_size"]) osd.font_size = n["font_size"].as<uint32_t>();
        }

        if (root["ota"]) {
            auto& n = root["ota"];
            if (n["server_url"]) ota.server_url = n["server_url"].as<std::string>();
            if (n["check_interval_sec"]) ota.check_interval_sec = n["check_interval_sec"].as<uint32_t>();
            if (n["certificate"]) ota.certificate = n["certificate"].as<std::string>();
            if (n["current_version"]) ota.current_version = n["current_version"].as<std::string>();
        }

        SPDLOG_INFO("Configuration loaded from {}", path);
        return true;

    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to load config: {}", e.what());
        return false;
    }
}

bool Config::save(const std::string& path) {
    try {
        YAML::Emitter out;
        out << YAML::BeginMap;

        out << YAML::Key << "system" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "device_id" << YAML::Value << system.device_id;
        out << YAML::Key << "log_level" << YAML::Value << system.log_level;
        out << YAML::Key << "log_file" << YAML::Value << system.log_file;
        out << YAML::Key << "log_max_size_mb" << YAML::Value << system.log_max_size_mb;
        out << YAML::Key << "log_max_files" << YAML::Value << system.log_max_files;
        out << YAML::EndMap;

        out << YAML::Key << "camera" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "device" << YAML::Value << camera.device;
        out << YAML::Key << "width" << YAML::Value << camera.width;
        out << YAML::Key << "height" << YAML::Value << camera.height;
        out << YAML::Key << "fps" << YAML::Value << camera.fps;
        out << YAML::Key << "input_format" << YAML::Value << camera.input_format;
        out << YAML::Key << "bitrate_kbps" << YAML::Value << camera.bitrate_kbps;
        out << YAML::EndMap;

        out << YAML::Key << "sensor" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "i2c_bus" << YAML::Value << sensor.i2c_bus;
        out << YAML::Key << "i2c_addr" << YAML::Value << static_cast<int>(sensor.i2c_addr);
        out << YAML::Key << "sample_interval_sec" << YAML::Value << sensor.sample_interval_sec;
        out << YAML::Key << "temp_threshold" << YAML::Value << sensor.temp_threshold;
        out << YAML::Key << "humidity_threshold" << YAML::Value << sensor.humidity_threshold;
        out << YAML::EndMap;

        out << YAML::Key << "streaming" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "rtsp_port" << YAML::Value << static_cast<int>(streaming.rtsp_port);
        out << YAML::Key << "stream_name" << YAML::Value << streaming.stream_name;
        out << YAML::Key << "max_clients" << YAML::Value << streaming.max_clients;
        out << YAML::EndMap;

        out << YAML::EndMap;

        std::ofstream fout(path);
        fout << out.c_str();

        SPDLOG_INFO("Configuration saved to {}", path);
        return true;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to save config: {}", e.what());
        return false;
    }
}

} // namespace smartcam
