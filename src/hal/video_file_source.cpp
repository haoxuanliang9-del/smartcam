#include "video_file_source.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <cstring>
#include <time.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
}

namespace smartcam {

static uint64_t wall_clock_us() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + ts.tv_nsec / 1000ULL;
}

VideoFileSource::~VideoFileSource() {
    stop();
    close();
}

bool VideoFileSource::open(const std::string& file_path) {
    file_path_ = file_path;
    //1.打开文件创建上下文
    if (avformat_open_input(&fmt_ctx_, file_path_.c_str(), nullptr, nullptr) < 0) {
        SPDLOG_ERROR("Cannot open video file: {}", file_path_);
        return false;
    }

    //2.读取文件初始化上下文
    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        SPDLOG_ERROR("Cannot find stream info in: {}", file_path_);
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    //3.从上下问获取流编号
    video_stream_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_idx_ < 0) {
        SPDLOG_ERROR("No video stream found in: {}", file_path_);
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    //4.获取流，开始编码
    AVStream* video_stream = fmt_ctx_->streams[video_stream_idx_];

    //5.获取编码器
    const AVCodec* codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!codec) {
        SPDLOG_ERROR("Unsupported codec in: {}", file_path_);
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    //6.创建编码器上下文
    decode_ctx_ = avcodec_alloc_context3(codec);

    //7.初始化编码器上下文参数
    avcodec_parameters_to_context(decode_ctx_, video_stream->codecpar);

    //8.打开编码器
    if (avcodec_open2(decode_ctx_, codec, nullptr) < 0) {
        SPDLOG_ERROR("Cannot open decoder for: {}", file_path_);
        avcodec_free_context(&decode_ctx_);
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    width_ = decode_ctx_->width;
    height_ = decode_ctx_->height;

    if (video_stream->r_frame_rate.num > 0 && video_stream->r_frame_rate.den > 0) {
        fps_ = video_stream->r_frame_rate.num / video_stream->r_frame_rate.den;
    }
    if (fps_ == 0) fps_ = 25;

    sws_ctx_ = sws_getContext(
        width_, height_, decode_ctx_->pix_fmt,
        width_, height_, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    opened_ = true;
    SPDLOG_INFO("VideoFileSource opened: {} ({}x{}@{}fps)", file_path_, width_, height_, fps_);
    return true;
}

void VideoFileSource::close() {
    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (decode_ctx_) { avcodec_free_context(&decode_ctx_); }
    if (fmt_ctx_) { avformat_close_input(&fmt_ctx_); }
    opened_ = false;
    SPDLOG_INFO("VideoFileSource closed");
}

bool VideoFileSource::start() {
    if (running_) return true;
    running_ = true;

    //防止虚假唤醒用的检查标志，消费者由于帧队列空而阻塞到被唤醒时，需要检查这个标志确认真的有新的帧
    frame_ready_ = false;
    read_thread_ = std::thread(&VideoFileSource::read_loop, this);
    SPDLOG_INFO("VideoFileSource started");
    return true;
}


//生产者线程read_loop退出，并通知消费者该退出了
void VideoFileSource::stop() {
    running_ = false;
    //唤醒由于帧队列空而阻塞的消费者，消费者检查到running_ = false就会进入退出流程
    frame_cv_.notify_all();
    if (read_thread_.joinable()) read_thread_.join();
    SPDLOG_INFO("VideoFileSource stopped");
}

bool VideoFileSource::seek_to_beginning() {
    avcodec_flush_buffers(decode_ctx_);
    if (av_seek_frame(fmt_ctx_, video_stream_idx_, 0, AVSEEK_FLAG_BACKWARD) < 0) {
        SPDLOG_WARN("Failed to seek to beginning, reopening file");
        avcodec_free_context(&decode_ctx_);
        avformat_close_input(&fmt_ctx_);

        fmt_ctx_ = nullptr;
        decode_ctx_ = nullptr;

        if (avformat_open_input(&fmt_ctx_, file_path_.c_str(), nullptr, nullptr) < 0) {
            SPDLOG_ERROR("Failed to reopen video file");
            return false;
        }
        avformat_find_stream_info(fmt_ctx_, nullptr);

        video_stream_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (video_stream_idx_ < 0) return false;

        const AVCodec* codec = avcodec_find_decoder(fmt_ctx_->streams[video_stream_idx_]->codecpar->codec_id);
        decode_ctx_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(decode_ctx_, fmt_ctx_->streams[video_stream_idx_]->codecpar);
        avcodec_open2(decode_ctx_, codec, nullptr);
    }
    SPDLOG_INFO("Video looped back to beginning");
    return true;
}

void VideoFileSource::read_loop() {
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    auto frame_duration = std::chrono::microseconds(1000000 / fps_);
    auto next_frame_time = std::chrono::steady_clock::now();

    while (running_) {
        int ret = av_read_frame(fmt_ctx_, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                if (seek_to_beginning()) {
                    av_packet_unref(pkt);
                    continue;
                }
            }
            SPDLOG_WARN("av_read_frame error, retrying in 1s...");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        if (pkt->stream_index != video_stream_idx_) {
            av_packet_unref(pkt);
            continue;
        }

        ret = avcodec_send_packet(decode_ctx_, pkt);
        av_packet_unref(pkt);

        if (ret < 0) continue;

        while (ret >= 0 && running_) {
            ret = avcodec_receive_frame(decode_ctx_, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            uint8_t* dst_data[3] = { nullptr, nullptr, nullptr };
            int dst_linesize[3] = {
                static_cast<int>(width_),
                static_cast<int>(width_ / 2),
                static_cast<int>(width_ / 2)
            };
            size_t yuv_size = width_ * height_ * 3 / 2;
            std::vector<uint8_t> yuv_buf(yuv_size);
            dst_data[0] = yuv_buf.data();
            dst_data[1] = yuv_buf.data() + width_ * height_;
            dst_data[2] = yuv_buf.data() + width_ * height_ * 5 / 4;

            sws_scale(sws_ctx_,
                      frame->data, frame->linesize, 0, height_,
                      dst_data, dst_linesize);

            RawFrame new_frame;
            new_frame.timestamp = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            new_frame.t_decode = wall_clock_us();
            new_frame.width = width_;
            new_frame.height = height_;
            new_frame.format = "YUV420P";
            new_frame.data = std::move(yuv_buf);

            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                latest_frame_ = std::move(new_frame);
                frame_ready_ = true;
                frame_cv_.notify_one();
            }

            av_frame_unref(frame);

            next_frame_time += frame_duration;
            auto now = std::chrono::steady_clock::now();
            if (next_frame_time > now) {
                std::this_thread::sleep_until(next_frame_time);
            } else {
                next_frame_time = now;
            }
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
}
/*
获取一个解码帧的接口
视频数据流的整个流程：解码——消费（OSD叠加）——编码——推流
解码——消费这个阶段使用单帧存储最新帧，编码——推流这个阶段使用队列存储最新帧。
原因是：
    - 解码——消费使用单帧是因为在正常情况下，消费速度很快，不会出现新帧到达旧帧还没取走的情况，所以不需要队列。
    - 而编码——推流这个阶段由于网络可能存在波动，所以必须用队列来缓解这个波动。
 另外，即使消费速度突然变慢导致旧帧没被取走新帧已经生成，也不应该保存旧帧。因为
    - 消费速度变慢，在视频观看者端会丢帧。如果保留并处理旧帧，那么客户端会看到的视频有有延迟。总的来说就是客户端视频会丢帧，且之后的视频都延迟播放
    - 但是如果新帧覆盖旧帧，那么客户端只会丢帧，而不会有延迟。
*/
bool VideoFileSource::get_frame(RawFrame& frame, int timeout_ms) {
    std::unique_lock<std::mutex> lock(frame_mutex_);
    /*
    wait_for的三个参数分别是，互斥锁unique_lock<mutex>，等待时间duration，等待结束条件predicate
    流程：
    （1）检查结束条件
        - 如果为true，wait_for返回true，if语句通过，向下执行；
        - 如果为false，释放锁，阻塞等待。最多等待duration时间，如果超过这个时间没有被唤醒则wait_for返回false。
    （2）阻塞时被唤醒，尝试重新获取锁，获取到锁后再次检查结束条件predicate（回到（1））
    */
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
