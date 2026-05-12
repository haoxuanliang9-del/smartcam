#include "camera_capture.h"
#include <spdlog/spdlog.h>
#include <opencv2/opencv.hpp>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libswscale/swscale.h>
#include <thread>
#include <cstring>

namespace smartcam {

CameraCapture::CameraCapture(std::shared_ptr<MessageQueue<Frame>> output_queue)
    : output_queue_(std::move(output_queue)) {}

CameraCapture::~CameraCapture() {
    stop();

    if (pkt_) av_packet_free(&pkt_);
    if (yuv_frame_) av_frame_free(&yuv_frame_);
    if (filter_frame_) av_frame_free(&filter_frame_);
    if (encoder_ctx_) avcodec_free_context(&encoder_ctx_);
    if (filter_graph_) avfilter_graph_free(&filter_graph_);
}

bool CameraCapture::init(const CameraConfig& config, const OsdConfig& osd_config) {
    camera_config_ = config;
    osd_config_ = osd_config;
    target_bitrate_ = config.bitrate_kbps;

    CameraHalConfig hal_config;
    hal_config.device = config.device;
    hal_config.width = config.width;
    hal_config.height = config.height;
    hal_config.fps = config.fps;
    hal_config.format = config.input_format;

    if (!camera_hal_.open(hal_config)) {
        SPDLOG_ERROR("Failed to open camera HAL");
        return false;
    }

    if (!init_encoder()) {
        SPDLOG_ERROR("Failed to initialize encoder");
        return false;
    }

    if (osd_config_.enabled) {
        if (!init_osd_filter()) {
            SPDLOG_WARN("Failed to initialize OSD filter, continuing without OSD");
            osd_config_.enabled = false;
        }
    }

    SPDLOG_INFO("CameraCapture initialized: {}x{}@{}fps, bitrate={}kbps",
                config.width, config.height, config.fps, config.bitrate_kbps);
    return true;
}

bool CameraCapture::init_encoder() {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        SPDLOG_ERROR("H264 encoder not found");
        return false;
    }

    encoder_ctx_ = avcodec_alloc_context3(codec);
    if (!encoder_ctx_) {
        SPDLOG_ERROR("Cannot allocate encoder context");
        return false;
    }

    encoder_ctx_->bit_rate = static_cast<int64_t>(camera_config_.bitrate_kbps) * 1000;
    encoder_ctx_->width = camera_config_.width;
    encoder_ctx_->height = camera_config_.height;
    encoder_ctx_->time_base = {1, static_cast<int>(camera_config_.fps)};
    encoder_ctx_->framerate = {static_cast<int>(camera_config_.fps), 1};
    encoder_ctx_->gop_size = static_cast<int>(camera_config_.fps * 2);
    encoder_ctx_->max_b_frames = 0;
    encoder_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder_ctx_->thread_count = 2;

    av_opt_set(encoder_ctx_->priv_data, "preset", "ultrafast", 0);
    av_opt_set(encoder_ctx_->priv_data, "tune", "zerolatency", 0);
    av_opt_set(encoder_ctx_->priv_data, "crf", "23", 0);

    if (avcodec_open2(encoder_ctx_, codec, nullptr) < 0) {
        SPDLOG_ERROR("Cannot open encoder");
        return false;
    }

    yuv_frame_ = av_frame_alloc();
    yuv_frame_->format = AV_PIX_FMT_YUV420P;
    yuv_frame_->width = camera_config_.width;
    yuv_frame_->height = camera_config_.height;
    av_frame_get_buffer(yuv_frame_, 0);

    filter_frame_ = av_frame_alloc();

    pkt_ = av_packet_alloc();

    SPDLOG_INFO("H264 encoder initialized (ultrafast/zerolatency)");
    return true;
}

bool CameraCapture::init_osd_filter() {
    filter_graph_ = avfilter_graph_alloc();
    if (!filter_graph_) {
        SPDLOG_ERROR("Cannot allocate filter graph");
        return false;
    }

    char args[512];
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1",
             camera_config_.width, camera_config_.height,
             AV_PIX_FMT_YUV420P,
             1, static_cast<int>(camera_config_.fps));

    int ret = avfilter_graph_create_filter(&buffersrc_ctx_,
                                           avfilter_get_by_name("buffer"),
                                           "in", args, nullptr, filter_graph_);
    if (ret < 0) {
        SPDLOG_ERROR("Cannot create buffer source filter");
        return false;
    }

    ret = avfilter_graph_create_filter(&buffersink_ctx_,
                                       avfilter_get_by_name("buffersink"),
                                       "out", nullptr, nullptr, filter_graph_);
    if (ret < 0) {
        SPDLOG_ERROR("Cannot create buffer sink filter");
        return false;
    }

    std::string drawtext_args = "text='':"
        "fontcolor=white:"
        "fontsize=" + std::to_string(osd_config_.font_size) + ":"
        "borderw=2:"
        "bordercolor=black:"
        "x=10:y=10";

    ret = avfilter_graph_create_filter(&drawtext_ctx_,
                                       avfilter_get_by_name("drawtext"),
                                       "drawtext", drawtext_args.c_str(),
                                       nullptr, filter_graph_);
    if (ret < 0) {
        SPDLOG_ERROR("Cannot create drawtext filter (fontconfig may be missing)");
        return false;
    }

    ret = avfilter_link(buffersrc_ctx_, 0, drawtext_ctx_, 0);
    if (ret < 0) {
        SPDLOG_ERROR("Cannot link buffer->drawtext");
        return false;
    }

    ret = avfilter_link(drawtext_ctx_, 0, buffersink_ctx_, 0);
    if (ret < 0) {
        SPDLOG_ERROR("Cannot link drawtext->buffersink");
        return false;
    }

    ret = avfilter_graph_config(filter_graph_, nullptr);
    if (ret < 0) {
        SPDLOG_ERROR("Cannot configure filter graph");
        return false;
    }

    SPDLOG_INFO("OSD filter initialized");
    return true;
}

void CameraCapture::start() {
    if (running_) return;

    if (!camera_hal_.start_capture()) {
        SPDLOG_ERROR("Failed to start camera capture");
        return;
    }

    running_ = true;
    std::thread(&CameraCapture::capture_loop, this).detach();
    SPDLOG_INFO("CameraCapture started");
}

void CameraCapture::stop() {
    running_ = false;
    camera_hal_.stop_capture();
    SPDLOG_INFO("CameraCapture stopped");
}

void CameraCapture::capture_loop() {
    RawFrame raw_frame;
    int64_t frame_count = 0;

    while (running_) {
        if (!camera_hal_.get_frame(raw_frame, 100)) {
            continue;
        }

        cv::Mat yuyv(raw_frame.height, raw_frame.width, CV_8UC2, raw_frame.data.data());
        cv::Mat bgr;
        cv::cvtColor(yuyv, bgr, cv::COLOR_YUV2BGR_YUYV);

        cv::Mat yuv_mat;
        cv::cvtColor(bgr, yuv_mat, cv::COLOR_BGR2YUV_I420);

        yuv_frame_->data[0] = yuv_mat.data;
        yuv_frame_->data[1] = yuv_mat.data + camera_config_.width * camera_config_.height;
        yuv_frame_->data[2] = yuv_mat.data + camera_config_.width * camera_config_.height * 5 / 4;
        yuv_frame_->linesize[0] = camera_config_.width;
        yuv_frame_->linesize[1] = camera_config_.width / 2;
        yuv_frame_->linesize[2] = camera_config_.width / 2;
        yuv_frame_->pts = frame_count++;

        {
            std::lock_guard<std::mutex> lock(osd_mutex_);
            if (osd_update_pending_ && drawtext_ctx_) {
                av_opt_set(drawtext_ctx_->priv, "text", osd_text_.c_str(), 0);
                osd_update_pending_ = false;
            }
        }

        if (osd_config_.enabled && filter_graph_) {
            if (av_buffersrc_add_frame_flags(buffersrc_ctx_, yuv_frame_,
                                              AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                SPDLOG_WARN("Failed to push frame to filter");
                continue;
            }
            if (av_buffersink_get_frame(buffersink_ctx_, filter_frame_) < 0) {
                SPDLOG_WARN("Failed to pull frame from filter");
                continue;
            }
            encode_frame(filter_frame_);
            av_frame_unref(filter_frame_);
        } else {
            encode_frame(yuv_frame_);
        }
    }
}

bool CameraCapture::encode_frame(AVFrame* frame) {
    uint32_t bitrate = target_bitrate_.load();
    if (encoder_ctx_->bit_rate != static_cast<int64_t>(bitrate) * 1000) {
        encoder_ctx_->bit_rate = static_cast<int64_t>(bitrate) * 1000;
    }

    int ret = avcodec_send_frame(encoder_ctx_, frame);
    if (ret < 0) return false;

    while (ret >= 0) {
        ret = avcodec_receive_packet(encoder_ctx_, pkt_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return false;

        Frame output;
        output.timestamp = static_cast<uint64_t>(pkt_->pts * av_q2d(encoder_ctx_->time_base) * 1000000);
        output.data = pkt_->data;
        output.size = pkt_->size;
        output.is_keyframe = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;

        output_queue_->push(output);
        av_packet_unref(pkt_);
    }
    return true;
}

void CameraCapture::set_bitrate(uint32_t bitrate_kbps) {
    target_bitrate_ = bitrate_kbps;
    SPDLOG_INFO("Bitrate set to {}kbps", bitrate_kbps);
}

void CameraCapture::update_osd_text(const std::string& text) {
    std::lock_guard<std::mutex> lock(osd_mutex_);
    osd_text_ = text;
    osd_update_pending_ = true;
}

} // namespace smartcam
