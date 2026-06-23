#include "v4l2_source.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/videodev2.h>

extern "C" {
#include <libswscale/swscale.h>
}

namespace smartcam {

static uint64_t wall_clock_us() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + ts.tv_nsec / 1000ULL;
}

V4l2Source::~V4l2Source() {
    stop();
    close();
}

bool V4l2Source::open(const std::string& device_path,
                       uint32_t requested_width,
                       uint32_t requested_height,
                       uint32_t requested_fps,
                       const std::string& pixel_format) {
    device_path_ = device_path;
    fps_ = requested_fps;

    v4l2_fd_ = ::open(device_path_.c_str(), O_RDWR | O_NONBLOCK);
    if (v4l2_fd_ < 0) {
        SPDLOG_ERROR("V4L2: cannot open device {}: {}", device_path_, strerror(errno));
        return false;
    }

    v4l2_capability cap = {};
    if (ioctl(v4l2_fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        SPDLOG_ERROR("V4L2: VIDIOC_QUERYCAP failed: {}", strerror(errno));
        ::close(v4l2_fd_); v4l2_fd_ = -1;
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        SPDLOG_ERROR("V4L2: device {} is not a video capture device", device_path_);
        ::close(v4l2_fd_); v4l2_fd_ = -1;
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        SPDLOG_ERROR("V4L2: device {} does not support streaming I/O", device_path_);
        ::close(v4l2_fd_); v4l2_fd_ = -1;
        return false;
    }

    v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(v4l2_fd_, VIDIOC_G_FMT, &fmt) < 0) {
        SPDLOG_ERROR("V4L2: VIDIOC_G_FMT failed: {}", strerror(errno));
        ::close(v4l2_fd_); v4l2_fd_ = -1;
        return false;
    }

    uint32_t requested_fourcc = V4L2_PIX_FMT_YUYV;
    src_pix_fmt_ = AV_PIX_FMT_YUYV422;

    if (!pixel_format.empty()) {
        if (pixel_format == "yuyv422" || pixel_format == "YUYV") {
            requested_fourcc = V4L2_PIX_FMT_YUYV;
            src_pix_fmt_ = AV_PIX_FMT_YUYV422;
        } else if (pixel_format == "mjpeg" || pixel_format == "MJPEG") {
            requested_fourcc = V4L2_PIX_FMT_MJPEG;
            src_pix_fmt_ = AV_PIX_FMT_YUVJ420P;
        } else if (pixel_format == "yuv420p" || pixel_format == "YU12") {
            requested_fourcc = V4L2_PIX_FMT_YUV420;
            src_pix_fmt_ = AV_PIX_FMT_YUV420P;
        } else {
            SPDLOG_WARN("V4L2: unknown pixel_format '{}', using YUYV", pixel_format);
        }
    }

    fmt.fmt.pix.pixelformat = requested_fourcc;
    fmt.fmt.pix.width = requested_width;
    fmt.fmt.pix.height = requested_height;

    if (ioctl(v4l2_fd_, VIDIOC_S_FMT, &fmt) < 0) {
        SPDLOG_ERROR("V4L2: VIDIOC_S_FMT failed for {}x{}@{}: {}",
                     requested_width, requested_height, pixel_format, strerror(errno));
        ::close(v4l2_fd_); v4l2_fd_ = -1;
        return false;
    }

    if (fmt.fmt.pix.pixelformat != requested_fourcc) {
        SPDLOG_WARN("V4L2: driver changed pixelformat from 0x{:x} to 0x{:x}",
                    requested_fourcc, fmt.fmt.pix.pixelformat);
        if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG) {
            src_pix_fmt_ = AV_PIX_FMT_YUVJ420P;
        }
    }

    width_ = fmt.fmt.pix.width;
    height_ = fmt.fmt.pix.height;
    v4l2_pixfmt_ = fmt.fmt.pix.pixelformat;

    v4l2_streamparm parm = {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(v4l2_fd_, VIDIOC_G_PARM, &parm) == 0 &&
        (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = requested_fps;
        if (ioctl(v4l2_fd_, VIDIOC_S_PARM, &parm) < 0) {
            SPDLOG_WARN("V4L2: VIDIOC_S_PARM failed: {}", strerror(errno));
        } else {
            uint32_t actual_fps = parm.parm.capture.timeperframe.denominator /
                                  parm.parm.capture.timeperframe.numerator;
            if (actual_fps != requested_fps) {
                SPDLOG_INFO("V4L2: fps adjusted by driver: {} -> {}", requested_fps, actual_fps);
            }
        }
    }

    v4l2_requestbuffers req = {};
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count = 4;
    if (ioctl(v4l2_fd_, VIDIOC_REQBUFS, &req) < 0) {
        SPDLOG_ERROR("V4L2: VIDIOC_REQBUFS failed: {}", strerror(errno));
        ::close(v4l2_fd_); v4l2_fd_ = -1;
        return false;
    }
    num_buffers_ = static_cast<int>(req.count);
    if (num_buffers_ < 2) {
        SPDLOG_ERROR("V4L2: driver only allocated {} buffers, need at least 2", num_buffers_);
        ::close(v4l2_fd_); v4l2_fd_ = -1;
        return false;
    }

    for (int i = 0; i < num_buffers_; i++) {
        v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = static_cast<uint32_t>(i);
        if (ioctl(v4l2_fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            SPDLOG_ERROR("V4L2: VIDIOC_QUERYBUF {} failed: {}", i, strerror(errno));
            ::close(v4l2_fd_); v4l2_fd_ = -1;
            return false;
        }
        mmap_buffer_sizes_[i] = buf.length;
        mmap_buffers_[i] = mmap(nullptr, buf.length,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                v4l2_fd_, buf.m.offset);
        if (mmap_buffers_[i] == MAP_FAILED) {
            SPDLOG_ERROR("V4L2: mmap buffer {} failed: {}", i, strerror(errno));
            ::close(v4l2_fd_); v4l2_fd_ = -1;
            return false;
        }
    }

    sws_ctx_ = sws_getContext(
        width_, height_, (AVPixelFormat)src_pix_fmt_,
        width_, height_, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    opened_ = true;
    SPDLOG_INFO("V4l2Source opened: {} ({}x{}@{}fps, pix_fmt=0x{:x}, buffers={})",
                device_path_, width_, height_, fps_, v4l2_pixfmt_, num_buffers_);
    return true;
}

void V4l2Source::close() {
    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }

    if (v4l2_fd_ >= 0) {
        for (int i = 0; i < num_buffers_; i++) {
            if (mmap_buffers_[i]) {
                munmap(mmap_buffers_[i], mmap_buffer_sizes_[i]);
                mmap_buffers_[i] = nullptr;
            }
        }
        ::close(v4l2_fd_);
        v4l2_fd_ = -1;
    }
    num_buffers_ = 0;
    output_frame_count_ = 0;
    first_output_ts_us_ = 0;
    opened_ = false;
    SPDLOG_INFO("V4l2Source closed");
}

bool V4l2Source::start() {
    if (running_) return true;
    if (!opened_) return false;
    running_ = true;
    frame_ready_ = false;
    capture_thread_ = std::thread(&V4l2Source::capture_loop, this);
    SPDLOG_INFO("V4l2Source started");
    return true;
}

void V4l2Source::stop() {
    running_ = false;
    frame_cv_.notify_all();
    if (capture_thread_.joinable()) capture_thread_.join();
    SPDLOG_INFO("V4l2Source stopped");
}

void V4l2Source::capture_loop() {
    for (int i = 0; i < num_buffers_; i++) {
        v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = static_cast<uint32_t>(i);
        if (ioctl(v4l2_fd_, VIDIOC_QBUF, &buf) < 0) {
            SPDLOG_ERROR("V4L2: VIDIOC_QBUF {} failed: {}", i, strerror(errno));
            return;
        }
    }

    int stream_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(v4l2_fd_, VIDIOC_STREAMON, &stream_type) < 0) {
        SPDLOG_ERROR("V4L2: VIDIOC_STREAMON failed: {}", strerror(errno));
        return;
    }

    auto frame_duration = std::chrono::microseconds(1000000 / std::max(fps_, 1U));
    auto next_frame_time = std::chrono::steady_clock::now();

    size_t yuv_size = width_ * height_ * 3 / 2;

    while (running_) {
        v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        int ret = ioctl(v4l2_fd_, VIDIOC_DQBUF, &buf);
        if (ret < 0) {
            if (errno == EAGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            SPDLOG_WARN("V4L2: VIDIOC_DQBUF error: {}", strerror(errno));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Check for hardware/driver-reported corrupted frame
        if (buf.flags & V4L2_BUF_FLAG_ERROR) {
            SPDLOG_WARN("V4L2: corrupted frame detected (flags=0x{:x}, seq={}), skipping",
                        buf.flags, buf.sequence);
            if (ioctl(v4l2_fd_, VIDIOC_QBUF, &buf) < 0) {
                SPDLOG_WARN("V4L2: VIDIOC_QBUF error on corrupted frame: {}", strerror(errno));
            }
            continue;
        }

        const uint8_t* src_data[1] = { static_cast<const uint8_t*>(mmap_buffers_[buf.index]) };
        int src_linesize[1] = { static_cast<int>(width_ * 2) };

        std::vector<uint8_t> yuv_buf(yuv_size);
        uint8_t* dst_data[3] = {
            yuv_buf.data(),
            yuv_buf.data() + width_ * height_,
            yuv_buf.data() + width_ * height_ * 5 / 4
        };
        int dst_linesize[3] = {
            static_cast<int>(width_),
            static_cast<int>(width_ / 2),
            static_cast<int>(width_ / 2)
        };

        sws_scale(sws_ctx_,
                  src_data, src_linesize, 0, height_,
                  dst_data, dst_linesize);

        uint64_t decode_us = wall_clock_us();

        if (ioctl(v4l2_fd_, VIDIOC_QBUF, &buf) < 0) {
            SPDLOG_WARN("V4L2: VIDIOC_QBUF error: {}", strerror(errno));
        }

        if (output_frame_count_ == 0) {
            first_output_ts_us_ = decode_us;
        }
        uint64_t output_ts_us = decode_us - first_output_ts_us_;
        output_frame_count_++;

        {
            RawFrame new_frame;
            new_frame.timestamp = output_ts_us;
            new_frame.width = width_;
            new_frame.height = height_;
            new_frame.format = "YUV420P";
            new_frame.data = std::move(yuv_buf);

            std::lock_guard<std::mutex> lock(frame_mutex_);
            latest_frame_ = std::move(new_frame);
            frame_ready_ = true;
            frame_cv_.notify_one();
        }

        next_frame_time += frame_duration;
        auto now = std::chrono::steady_clock::now();
        if (next_frame_time > now) {
            std::this_thread::sleep_until(next_frame_time);
        } else {
            next_frame_time = now;
        }
    }

    int stream_off = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(v4l2_fd_, VIDIOC_STREAMOFF, &stream_off);
}

bool V4l2Source::get_frame(RawFrame& frame, int timeout_ms) {
    std::unique_lock<std::mutex> lock(frame_mutex_);
    if (frame_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [this] { return frame_ready_ || !running_; })) {
        if (frame_ready_) {
            frame = std::move(latest_frame_);
            frame_ready_ = false;
            return true;
        }
    }
    return false;
}

} // namespace smartcam
