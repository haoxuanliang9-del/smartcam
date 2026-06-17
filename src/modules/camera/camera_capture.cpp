#include "camera_capture.h"
#include <spdlog/spdlog.h>
#include <time.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libswscale/swscale.h>
#include <libavcodec/bsf.h>
}

#include <thread>
#include <cstring>
#include <unistd.h>
#include <algorithm>

namespace smartcam {

CameraCapture::~CameraCapture() {
    stop();

    if (pkt_) av_packet_free(&pkt_);
    if (yuv_frame_) av_frame_free(&yuv_frame_);
    if (filter_frame_) av_frame_free(&filter_frame_);
    if (encoder_ctx_) avcodec_free_context(&encoder_ctx_);
    if (filter_graph_) avfilter_graph_free(&filter_graph_);
    if (annexb_bsf_) av_bsf_free(&annexb_bsf_);
    if (!osd_textfile_path_.empty()) {
        unlink(osd_textfile_path_.c_str());
    }
}

bool CameraCapture::init(const CameraConfig& config, const OsdConfig& osd_config) {
    osd_config_ = osd_config;
    target_bitrate_ = config.bitrate_kbps;

    if (!v4l2_source_.open(config.v4l2_device,
                           config.v4l2_width, config.v4l2_height,
                           config.v4l2_fps, config.v4l2_pix_fmt)) {
        SPDLOG_ERROR("Failed to open V4L2 device: {}", config.v4l2_device);
        return false;
    }
    width_ = v4l2_source_.width();
    height_ = v4l2_source_.height();
    fps_ = v4l2_source_.fps();
    SPDLOG_INFO("CameraCapture using V4L2 source: {}", config.v4l2_device);

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
                width_, height_, fps_, config.bitrate_kbps);
    return true;
}

bool CameraCapture::init_encoder() {
    last_bitrate_time_ = std::chrono::steady_clock::now();

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
    encoder_ctx_->bit_rate = static_cast<int64_t>(target_bitrate_.load()) * 1000;
    encoder_ctx_->rc_max_rate = static_cast<int64_t>(target_bitrate_.load()) * 1000 * 15 / 10;
    encoder_ctx_->rc_buffer_size = static_cast<int64_t>(target_bitrate_.load()) * 1000 / 10;
    encoder_ctx_->width = width_;
    encoder_ctx_->height = height_;
    encoder_ctx_->time_base = {1, static_cast<int>(fps_)};
    encoder_ctx_->framerate = {static_cast<int>(fps_), 1};
    encoder_ctx_->gop_size = std::max(1, static_cast<int>(fps_ / 2));
    encoder_ctx_->max_b_frames = 0;
    encoder_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder_ctx_->thread_count = 1;
    encoder_ctx_->delay = 0;
    encoder_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;

    av_opt_set(encoder_ctx_->priv_data, "preset", "ultrafast", 0);
    av_opt_set(encoder_ctx_->priv_data, "tune", "zerolatency", 0);
    av_opt_set(encoder_ctx_->priv_data, "profile", "baseline", 0);
    av_opt_set(encoder_ctx_->priv_data, "rc-lookahead", "0", 0);
    av_opt_set(encoder_ctx_->priv_data, "sync-lookahead", "0", 0);
    av_opt_set(encoder_ctx_->priv_data, "sliced-threads", "0", 0);
    av_opt_set(encoder_ctx_->priv_data, "repeat-headers", "1", 0);
    av_opt_set(encoder_ctx_->priv_data, "vbv-maxrate",
               std::to_string(target_bitrate_.load() * 15 / 10).c_str(), 0);
    av_opt_set(encoder_ctx_->priv_data, "vbv-bufsize",
               std::to_string(target_bitrate_.load() / 10).c_str(), 0);

    if (avcodec_open2(encoder_ctx_, codec, nullptr) < 0) {
        SPDLOG_ERROR("Cannot open encoder");
        return false;
    }

    const AVBitStreamFilter* bsf = av_bsf_get_by_name("h264_mp4toannexb");
    if (bsf) {
        int ret = av_bsf_alloc(bsf, &annexb_bsf_);
        if (ret >= 0) {
            avcodec_parameters_from_context(annexb_bsf_->par_in, encoder_ctx_);
            ret = av_bsf_init(annexb_bsf_);
            if (ret < 0) {
                SPDLOG_WARN("Failed to init h264_mp4toannexb BSF");
                av_bsf_free(&annexb_bsf_);
                annexb_bsf_ = nullptr;
            } else {
                SPDLOG_INFO("H264 Annex-B BSF initialized");
            }
        }
    }

    yuv_frame_ = av_frame_alloc();
    yuv_frame_->format = AV_PIX_FMT_YUV420P;
    yuv_frame_->width = width_;
    yuv_frame_->height = height_;
    av_frame_get_buffer(yuv_frame_, 0);

    filter_frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();

    SPDLOG_INFO("H264 encoder initialized (ultrafast/zerolatency, gop={}, rc_buf={}kbits, threads=1)",
                encoder_ctx_->gop_size, encoder_ctx_->rc_buffer_size / 1000);
    return true;
}

bool CameraCapture::init_osd_filter() {
    osd_textfile_path_ = "/tmp/smartcam_osd.txt";
    {
        std::ofstream ofs(osd_textfile_path_);
        ofs << "Initializing..." << std::endl;
    }

    filter_graph_ = avfilter_graph_alloc();
    if (!filter_graph_) {
        SPDLOG_ERROR("Cannot allocate filter graph");
        return false;
    }

    char args[512];
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1",
             width_, height_,
             AV_PIX_FMT_YUV420P,
             1, static_cast<int>(fps_));

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

    std::string drawtext_args =
        "textfile='" + osd_textfile_path_ + "':"
        "reload=1:"
        "fontcolor=white:"
        "fontsize=" + std::to_string(osd_config_.font_size) + ":"
        "borderw=2:"
        "bordercolor=black:"
        "x=10:y=10:"
        "expansion=none";
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

    SPDLOG_INFO("OSD filter initialized (textfile={}, reload=1)", osd_textfile_path_);
    return true;
}

void CameraCapture::start() {
    if (running_) return;

    if (!v4l2_source_.start()) {
        SPDLOG_ERROR("Failed to start V4L2 source");
        return;
    }

    running_ = true;
    std::thread(&CameraCapture::capture_loop, this).detach();
    SPDLOG_INFO("CameraCapture started");
}

void CameraCapture::stop() {
    running_ = false;
    v4l2_source_.stop();
    SPDLOG_INFO("CameraCapture stopped");
}

void CameraCapture::capture_loop() {
    RawFrame raw_frame;
    int64_t frame_count = 0;
    uint32_t frame_seq = 0;

    while (running_) {
        if (!v4l2_source_.get_frame(raw_frame, 100)) {
            continue;
        }

        yuv_frame_->data[0] = raw_frame.data.data();
        yuv_frame_->data[1] = raw_frame.data.data() + width_ * height_;
        yuv_frame_->data[2] = raw_frame.data.data() + width_ * height_ * 5 / 4;
        yuv_frame_->linesize[0] = width_;
        yuv_frame_->linesize[1] = width_ / 2;
        yuv_frame_->linesize[2] = width_ / 2;
        yuv_frame_->pts = frame_count++;

        uint32_t current_seq = ++frame_seq;

        bool force_idr_now = request_idr_.exchange(false, std::memory_order_acq_rel);
        if (force_idr_now) {
            yuv_frame_->pict_type = AV_PICTURE_TYPE_I;
            av_opt_set(encoder_ctx_->priv_data, "force-idr", "1", 0);
            SPDLOG_INFO("Forced IDR requested, next frame will be IDR");
        } else {
            yuv_frame_->pict_type = AV_PICTURE_TYPE_NONE;
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
            encode_frame(filter_frame_, current_seq, raw_frame.timestamp);
            av_frame_unref(filter_frame_);
        } else {
            encode_frame(yuv_frame_, current_seq, raw_frame.timestamp);
        }

        if (!force_idr_now) {
            av_opt_set(encoder_ctx_->priv_data, "force-idr", "0", 0);
        }
    }
}

bool CameraCapture::encode_frame(AVFrame* frame, uint32_t frame_seq, uint64_t capture_ts) {
    uint32_t bitrate = target_bitrate_.load();
    if (encoder_ctx_->bit_rate != static_cast<int64_t>(bitrate) * 1000) {
        encoder_ctx_->bit_rate = static_cast<int64_t>(bitrate) * 1000;
        encoder_ctx_->rc_max_rate = static_cast<int64_t>(bitrate) * 1000 * 15 / 10;
        encoder_ctx_->rc_buffer_size = static_cast<int64_t>(bitrate) * 1000 / 10;
        av_opt_set(encoder_ctx_->priv_data, "vbv-maxrate",
                   std::to_string(bitrate * 15 / 10).c_str(), 0);
        av_opt_set(encoder_ctx_->priv_data, "vbv-bufsize",
                   std::to_string(bitrate / 10).c_str(), 0);
    }

    int ret = avcodec_send_frame(encoder_ctx_, frame);
    if (ret < 0) return false;

    while (ret >= 0) {
        ret = avcodec_receive_packet(encoder_ctx_, pkt_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return false;

        AVPacket* send_pkt = pkt_;
        AVPacket* bsf_pkt = nullptr;

        if (annexb_bsf_) {
            bsf_pkt = av_packet_alloc();
            av_packet_ref(bsf_pkt, pkt_);
            int bsf_ret = av_bsf_send_packet(annexb_bsf_, bsf_pkt);
            if (bsf_ret >= 0) {
                bsf_ret = av_bsf_receive_packet(annexb_bsf_, bsf_pkt);
            }
            if (bsf_ret >= 0) {
                send_pkt = bsf_pkt;
            } else {
                av_packet_free(&bsf_pkt);
                bsf_pkt = nullptr;
            }
        }

        Frame output;
        output.timestamp = capture_ts;
        output.data.assign(send_pkt->data, send_pkt->data + send_pkt->size);
        output.is_keyframe = (send_pkt->flags & AV_PKT_FLAG_KEY) != 0;

        bytes_encoded_ += output.data.size();
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_bitrate_time_).count();
        if (elapsed >= 2) {
            uint32_t kbps = static_cast<uint32_t>((bytes_encoded_ * 8) / (elapsed * 1000));
            actual_bitrate_kbps_ = kbps;
            bytes_encoded_ = 0;
            last_bitrate_time_ = now;
            SPDLOG_INFO("Actual bitrate: {} kbps", kbps);
            if (actual_bitrate_cb_) actual_bitrate_cb_(kbps);
        }

        auto shared_frame = std::make_shared<Frame>(std::move(output));

        {
            for (auto& slot : client_slots_) {
                slot->put(shared_frame);
            }
        }

        if (bsf_pkt) {
            av_packet_free(&bsf_pkt);
        }
        av_packet_unref(pkt_);
    }
    return true;
}

void CameraCapture::set_bitrate(uint32_t bitrate_kbps) {
    target_bitrate_ = bitrate_kbps;
    SPDLOG_INFO("Bitrate set to {}kbps", bitrate_kbps);
}

void CameraCapture::update_osd_text(const std::string& text) {
    if (osd_textfile_path_.empty()) return;

    std::string tmp_path = osd_textfile_path_ + ".tmp";
    {
        std::ofstream ofs(tmp_path);
        if (!ofs) {
            SPDLOG_WARN("Failed to write OSD textfile");
            return;
        }
        ofs << text << std::endl;
    }
    rename(tmp_path.c_str(), osd_textfile_path_.c_str());
}

void CameraCapture::add_client_queue(std::shared_ptr<LatestValue<Frame>> slot) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    client_slots_.push_back(slot);
    SPDLOG_INFO("Client slot added, total: {}", client_slots_.size());
}

void CameraCapture::remove_client_queue(std::shared_ptr<LatestValue<Frame>> slot) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = std::remove(client_slots_.begin(), client_slots_.end(), slot);
    if (it != client_slots_.end()) {
        client_slots_.erase(it, client_slots_.end());
        SPDLOG_INFO("Client slot removed, remaining: {}", client_slots_.size());
    }
}

} // namespace smartcam
