#ifndef SMARTCAM_COMMON_TYPES_H
#define SMARTCAM_COMMON_TYPES_H

#include <cstdint>
#include <string>
#include <chrono>

namespace smartcam {

using Timestamp = std::chrono::microseconds;
using Clock = std::chrono::steady_clock;

struct Frame {
    uint64_t timestamp;
    uint8_t* data;
    size_t size;
    bool is_keyframe;
};

struct SensorData {
    uint64_t timestamp;
    float temperature;
    float humidity;
};

struct EnvironmentChange {
    bool significant;
    float temp_delta;
    float humidity_delta;
};

struct MotionEvent {
    uint64_t timestamp;
    bool detected;
    int motion_area;
};

} // namespace smartcam

#endif // SMARTCAM_COMMON_TYPES_H
