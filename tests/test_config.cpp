#include <gtest/gtest.h>
#include "common/config.h"
#include <fstream>

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_config_ = R"(
system:
  device_id: "TEST-CAM"
  log_level: "debug"
  log_file: "/tmp/test.log"
  log_max_size_mb: 1
  log_max_files: 1

camera:
  device: "/dev/video0"
  width: 1280
  height: 720
  fps: 15
  input_format: "YUYV"
  bitrate_kbps: 1000

sensor:
  i2c_bus: 1
  i2c_addr: 0x38
  sample_interval_sec: 2
  temp_threshold: 1.5
  humidity_threshold: 3.0

streaming:
  rtsp_port: 8554
  stream_name: "test"
  max_clients: 5

analysis:
  motion_detection:
    enabled: true
    sensitivity: 0.5
    min_area: 300
    check_interval_ms: 100
  bitrate_adaptation:
    enabled: false
    boost_factor: 2.0
    recovery_time_sec: 60

osd:
  enabled: false
  position: "top-left"
  font_size: 16

ota:
  server_url: ""
  check_interval_sec: 7200
  certificate: ""
  current_version: "0.1.0"
)";
        test_path_ = "/tmp/smartcam_test_config.yaml";
        std::ofstream(test_path_) << test_config_;
    }

    void TearDown() override {
        std::remove(test_path_.c_str());
    }

    std::string test_config_;
    std::string test_path_;
};

TEST_F(ConfigTest, LoadFromFile) {
    smartcam::Config config;
    ASSERT_TRUE(config.load(test_path_));

    EXPECT_EQ(config.system.device_id, "TEST-CAM");
    EXPECT_EQ(config.system.log_level, "debug");
    EXPECT_EQ(config.camera.width, 1280);
    EXPECT_EQ(config.camera.height, 720);
    EXPECT_EQ(config.camera.fps, 15);
    EXPECT_EQ(config.camera.bitrate_kbps, 1000);
    EXPECT_EQ(config.sensor.i2c_bus, 1);
    EXPECT_EQ(config.sensor.i2c_addr, 0x38);
    EXPECT_EQ(config.sensor.sample_interval_sec, 2);
    EXPECT_FLOAT_EQ(config.sensor.temp_threshold, 1.5f);
    EXPECT_FLOAT_EQ(config.sensor.humidity_threshold, 3.0f);
    EXPECT_EQ(config.streaming.rtsp_port, 8554);
    EXPECT_EQ(config.streaming.stream_name, "test");
    EXPECT_EQ(config.streaming.max_clients, 5u);
}

TEST_F(ConfigTest, DefaultValues) {
    smartcam::Config config;
    EXPECT_EQ(config.camera.width, 1920);
    EXPECT_EQ(config.camera.height, 1080);
    EXPECT_EQ(config.camera.fps, 30);
    EXPECT_EQ(config.camera.bitrate_kbps, 2000u);
    EXPECT_EQ(config.sensor.i2c_bus, 1);
    EXPECT_EQ(config.streaming.rtsp_port, 8554);
}

TEST_F(ConfigTest, LoadNonexistentFile) {
    smartcam::Config config;
    EXPECT_FALSE(config.load("/nonexistent/path/config.yaml"));
}

TEST_F(ConfigTest, AnalysisConfig) {
    smartcam::Config config;
    ASSERT_TRUE(config.load(test_path_));

    EXPECT_TRUE(config.analysis.motion_detection.enabled);
    EXPECT_FLOAT_EQ(config.analysis.motion_detection.sensitivity, 0.5f);
    EXPECT_EQ(config.analysis.motion_detection.min_area, 300);
    EXPECT_FALSE(config.analysis.bitrate_adaptation.enabled);
    EXPECT_FLOAT_EQ(config.analysis.bitrate_adaptation.boost_factor, 2.0f);
}
