#include <gtest/gtest.h>
#include "modules/sensor/sensor_module.h"

class SensorModuleTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto queue = std::make_shared<smartcam::MessageQueue<smartcam::SensorData>>(64);
        sensor_ = std::make_unique<smartcam::SensorModule>(queue);
    }

    std::unique_ptr<smartcam::SensorModule> sensor_;
};

TEST_F(SensorModuleTest, InitFailsWithoutDevice) {
    smartcam::SensorConfig config;
    config.i2c_bus = 99;
    config.i2c_addr = 0x38;
    EXPECT_FALSE(sensor_->init(config));
}

TEST_F(SensorModuleTest, NotRunningByDefault) {
    EXPECT_FALSE(sensor_->is_running());
}
