#include "sensor_module.h"
#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>
#include <cstring>

namespace smartcam {

static constexpr uint8_t AHT20_CMD_INIT[]        = {0xBE, 0x08, 0x00};
static constexpr uint8_t AHT20_CMD_MEASURE[]     = {0xAC, 0x33, 0x00};
static constexpr uint8_t AHT20_CMD_RESET[]       = {0xBA};
static constexpr uint8_t AHT20_CMD_CALIBRATE[]   = {0xE1, 0x08, 0x00};

SensorModule::SensorModule(std::shared_ptr<MessageQueue<SensorData>> output_queue)
    : output_queue_(std::move(output_queue)) {}

SensorModule::~SensorModule() {
    stop();
}

bool SensorModule::init(const SensorConfig& config) {
    config_ = config;

    I2cHalConfig hal_config;
    hal_config.bus = config.i2c_bus;
    hal_config.addr = config.i2c_addr;

    if (!i2c_hal_.open(hal_config)) {
        SPDLOG_ERROR("Failed to open I2C for AHT20");
        return false;
    }

    if (!check_calibration()) {
        SPDLOG_INFO("AHT20 not calibrated, sending init command");
        if (!i2c_hal_.write(AHT20_CMD_INIT, sizeof(AHT20_CMD_INIT))) {
            SPDLOG_ERROR("AHT20 init command failed");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!trigger_measurement()) {
        SPDLOG_ERROR("AHT20 initial measurement trigger failed");
        return false;
    }

    SPDLOG_INFO("AHT20 sensor initialized (bus={}, addr=0x{:02x})",
                config.i2c_bus, config.i2c_addr);
    return true;
}

void SensorModule::start() {
    if (running_) return;
    running_ = true;
    std::thread(&SensorModule::sample_loop, this).detach();
    SPDLOG_INFO("SensorModule started (interval={}s)", config_.sample_interval_sec);
}

void SensorModule::stop() {
    running_ = false;
    SPDLOG_INFO("SensorModule stopped");
}

void SensorModule::sample_loop() {
    while (running_) {
        SensorData data;
        if (read(data)) {
            last_data_ = data;
            output_queue_->push(data);
            if (data_cb_) data_cb_(data.temperature, data.humidity);
        }

        std::this_thread::sleep_for(
            std::chrono::seconds(config_.sample_interval_sec));
    }
}

bool SensorModule::read(SensorData& data) {
    if (!trigger_measurement()) {
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    return read_measurement(data);
}

bool SensorModule::trigger_measurement() {
    return i2c_hal_.write(AHT20_CMD_MEASURE, sizeof(AHT20_CMD_MEASURE));
}

bool SensorModule::read_measurement(SensorData& data) {
    uint8_t buf[7] = {};
    if (!i2c_hal_.read(buf, sizeof(buf))) {
        SPDLOG_WARN("AHT20 read failed");
        return false;
    }

    if (buf[0] & 0x80) {
        SPDLOG_WARN("AHT20 busy, retrying");
        return false;
    }

    uint32_t raw_humidity = (static_cast<uint32_t>(buf[1]) << 12) |
                            (static_cast<uint32_t>(buf[2]) << 4) |
                            (static_cast<uint32_t>(buf[3]) >> 4);

    uint32_t raw_temp = ((static_cast<uint32_t>(buf[3]) & 0x0F) << 16) |
                        (static_cast<uint32_t>(buf[4]) << 8) |
                        static_cast<uint32_t>(buf[5]);

    data.humidity = (static_cast<float>(raw_humidity) / 1048576.0f) * 100.0f;
    data.temperature = (static_cast<float>(raw_temp) / 1048576.0f) * 200.0f - 50.0f;
    data.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    SPDLOG_INFO("AHT20 read: T={:.1f}C H={:.1f}%", data.temperature, data.humidity);
    return true;
}

bool SensorModule::check_calibration() {
    uint8_t status = 0;
    if (!i2c_hal_.read(&status, 1)) {
        return false;
    }
    return (status & 0x08) != 0;
}

} // namespace smartcam
