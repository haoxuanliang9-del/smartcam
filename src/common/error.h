#ifndef SMARTCAM_COMMON_ERROR_H
#define SMARTCAM_COMMON_ERROR_H

#include <string>
#include <stdexcept>

namespace smartcam {

enum class ErrorCode {
    SUCCESS = 0,
    CAMERA_OPEN_FAILED = 1,
    CAMERA_INIT_FAILED = 2,
    CAMERA_FORMAT_FAILED = 3,
    SENSOR_INIT_FAILED = 100,
    SENSOR_READ_FAILED = 101,
    ENCODER_INIT_FAILED = 200,
    ENCODER_ENCODE_FAILED = 201,
    RTSP_START_FAILED = 300,
    OTA_CHECK_FAILED = 400,
    OTA_DOWNLOAD_FAILED = 401,
    OTA_VERIFY_FAILED = 402,
    OTA_INSTALL_FAILED = 403,
    CONFIG_LOAD_FAILED = 500,
    CONFIG_PARSE_FAILED = 501,
};

class Error {
public:
    Error(ErrorCode code, const std::string& message)
        : code_(code), message_(message) {}

    ErrorCode code() const { return code_; }
    const std::string& message() const { return message_; }

    bool is_fatal() const {
        return static_cast<int>(code_) > 0 && static_cast<int>(code_) < 100;
    }

    bool is_severe() const {
        return static_cast<int>(code_) >= 100 && static_cast<int>(code_) < 200;
    }

    bool ok() const { return code_ == ErrorCode::SUCCESS; }

private:
    ErrorCode code_;
    std::string message_;
};

} // namespace smartcam

#endif // SMARTCAM_COMMON_ERROR_H
