#include <gtest/gtest.h>
#include "modules/analysis/intelligent_analysis.h"

class AnalysisTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto sensor_queue = std::make_shared<smartcam::MessageQueue<smartcam::SensorData>>(64);
        auto frame_queue = std::make_shared<smartcam::MessageQueue<smartcam::Frame>>(64);
        analysis_ = std::make_unique<smartcam::IntelligentAnalysis>(sensor_queue, frame_queue);
    }

    std::unique_ptr<smartcam::IntelligentAnalysis> analysis_;
};

TEST_F(AnalysisTest, InitWithDefaults) {
    smartcam::AnalysisConfig config;
    EXPECT_TRUE(analysis_->init(config, 2000));
    EXPECT_EQ(analysis_->get_recommended_bitrate(), 2000u);
}

TEST_F(AnalysisTest, NotRunningByDefault) {
    EXPECT_FALSE(analysis_->is_running());
}

TEST_F(AnalysisTest, NoMotionByDefault) {
    smartcam::AnalysisConfig config;
    analysis_->init(config, 2000);
    EXPECT_FALSE(analysis_->is_motion_detected());
}
