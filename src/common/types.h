#ifndef SMARTCAM_COMMON_TYPES_H
#define SMARTCAM_COMMON_TYPES_H

#include <cstdint>
#include <string>
#include <vector>

namespace smartcam {

struct Frame {
    uint64_t timestamp = 0;
    std::vector<uint8_t> data;
    bool is_keyframe = false;

    uint64_t t_decode = 0;
    uint64_t t_encode_in = 0;
    uint64_t t_encode_out = 0;
    uint64_t t_queue_push = 0;
    uint64_t t_rtp_send = 0;
    uint32_t frame_seq = 0;
};

struct RawFrame {
    uint64_t timestamp = 0;
    std::vector<uint8_t> data;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string format;

    uint64_t t_decode = 0;
};

struct AudioFrame {
    uint64_t timestamp = 0;        // 微秒
    std::vector<uint8_t> data;     // G.711 PCMU 编码数据
};

struct SensorData {
    uint64_t timestamp = 0;
    float temperature = 0.0f;
    float humidity = 0.0f;
};

} // namespace smartcam

#endif // SMARTCAM_COMMON_TYPES_H
