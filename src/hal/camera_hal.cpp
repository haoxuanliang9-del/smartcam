#include "camera_hal.h"
#include <spdlog/spdlog.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <cstring>
#include <errno.h>

namespace smartcam {

CameraHal::~CameraHal() {
    stop_capture();
    close();
}

bool CameraHal::open(const CameraHalConfig& config) {
    config_ = config;

    fd_ = ::open(config_.device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        SPDLOG_ERROR("Cannot open camera device {}: {}", config_.device, strerror(errno));
        return false;
    }

    if (!init_device()) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    SPDLOG_INFO("Camera HAL opened: {} ({}x{}@{}fps)",
                config_.device, config_.width, config_.height, config_.fps);
    return true;
}

void CameraHal::close() {
    if (fd_ >= 0) {
        for (auto* buf : buffers_) {
            if (buf) munmap(buf, 0);
        }
        buffers_.clear();
        ::close(fd_);
        fd_ = -1;
        SPDLOG_INFO("Camera HAL closed");
    }
}

bool CameraHal::init_device() {
    struct v4l2_capability cap;
    if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        SPDLOG_ERROR("VIDIOC_QUERYCAP failed: {}", strerror(errno));
        return false;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        SPDLOG_ERROR("Device does not support video capture");
        return false;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        SPDLOG_ERROR("Device does not support streaming");
        return false;
    }

    if (!set_format()) return false;
    if (!request_buffers()) return false;

    return true;
}

bool CameraHal::set_format() {
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = config_.width;
    fmt.fmt.pix.height = config_.height;

    if (config_.format == "YUYV") {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    } else if (config_.format == "NV12") {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    } else if (config_.format == "MJPEG") {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    } else {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    }
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        SPDLOG_ERROR("VIDIOC_S_FMT failed: {}", strerror(errno));
        return false;
    }

    if (fmt.fmt.pix.width != config_.width || fmt.fmt.pix.height != config_.height) {
        SPDLOG_WARN("Camera resolution adjusted: {}x{} -> {}x{}",
                    config_.width, config_.height,
                    fmt.fmt.pix.width, fmt.fmt.pix.height);
        config_.width = fmt.fmt.pix.width;
        config_.height = fmt.fmt.pix.height;
    }

    struct v4l2_streamparm parm = {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = config_.fps;

    if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
        SPDLOG_WARN("Failed to set frame rate: {}", strerror(errno));
    }

    return true;
}

bool CameraHal::request_buffers() {
    struct v4l2_requestbuffers req = {};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        SPDLOG_ERROR("VIDIOC_REQBUFS failed: {}", strerror(errno));
        return false;
    }

    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            SPDLOG_ERROR("VIDIOC_QUERYBUF failed: {}", strerror(errno));
            return false;
        }

        void* ptr = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd_, buf.m.offset);
        if (ptr == MAP_FAILED) {
            SPDLOG_ERROR("mmap failed: {}", strerror(errno));
            return false;
        }

        buffers_.push_back(ptr);
    }

    return true;
}

bool CameraHal::start_capture() {
    for (size_t i = 0; i < buffers_.size(); i++) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = static_cast<uint32_t>(i);

        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            SPDLOG_ERROR("VIDIOC_QBUF failed: {}", strerror(errno));
            return false;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        SPDLOG_ERROR("VIDIOC_STREAMON failed: {}", strerror(errno));
        return false;
    }

    capturing_ = true;
    SPDLOG_INFO("Camera capture started");
    return true;
}

void CameraHal::stop_capture() {
    if (capturing_ && fd_ >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_, VIDIOC_STREAMOFF, &type);
        capturing_ = false;
        SPDLOG_INFO("Camera capture stopped");
    }
}

void CameraHal::set_frame_callback(FrameCallback cb) {
    callback_ = std::move(cb);
}

bool CameraHal::get_frame(RawFrame& frame, int timeout_ms) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);

    struct timeval tv = {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
    if (ret < 0) {
        if (errno == EINTR) return false;
        SPDLOG_ERROR("select() failed: {}", strerror(errno));
        return false;
    }
    if (ret == 0) return false;

    struct v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        SPDLOG_ERROR("VIDIOC_DQBUF failed: {}", strerror(errno));
        return false;
    }

    frame.timestamp = static_cast<uint64_t>(buf.timestamp.tv_sec) * 1000000 +
                      buf.timestamp.tv_usec;
    frame.width = config_.width;
    frame.height = config_.height;
    frame.format = config_.format;

    size_t data_size = buf.bytesused;
    frame.data.resize(data_size);
    if (buf.index < buffers_.size()) {
        std::memcpy(frame.data.data(), buffers_[buf.index], data_size);
    }

    if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        SPDLOG_ERROR("VIDIOC_QBUF (requeue) failed: {}", strerror(errno));
        return false;
    }

    if (callback_) {
        callback_(frame);
    }

    return true;
}

} // namespace smartcam
