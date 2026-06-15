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

static uint64_t wall_clock_us() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + ts.tv_nsec / 1000ULL;
}

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
    bitrate_kbps_ = config.bitrate_kbps;
    target_bitrate_ = config.bitrate_kbps;

    if (!video_source_.open(config.video_file)) {
        SPDLOG_ERROR("Failed to open video file: {}", config.video_file);
        return false;
    }

    width_ = video_source_.width();
    height_ = video_source_.height();
    fps_ = video_source_.fps();

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

    SPDLOG_INFO("CameraCapture initialized: {}x{}@{}fps, bitrate={}kbps, file={}",
                width_, height_, fps_, bitrate_kbps_, config.video_file);
    return true;
}

bool CameraCapture::init_encoder() {
    last_bitrate_time_ = std::chrono::steady_clock::now();

    //1.获取解码器
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        SPDLOG_ERROR("H264 encoder not found");
        return false;
    }
    //2.创建编码器上下文
    encoder_ctx_ = avcodec_alloc_context3(codec);
    if (!encoder_ctx_) {
        SPDLOG_ERROR("Cannot allocate encoder context");
        return false;
    }
    //3.1.设置编码器上下文参数
    encoder_ctx_->bit_rate = static_cast<int64_t>(bitrate_kbps_) * 1000;
    encoder_ctx_->rc_max_rate = static_cast<int64_t>(bitrate_kbps_) * 1000;
    encoder_ctx_->rc_buffer_size = static_cast<int64_t>(bitrate_kbps_) * 1000 / fps_;  // 1帧的比特数
    encoder_ctx_->width = width_;
    encoder_ctx_->height = height_;
    encoder_ctx_->time_base = {1, static_cast<int>(fps_)};
    encoder_ctx_->framerate = {static_cast<int>(fps_), 1};
    encoder_ctx_->gop_size = static_cast<int>(fps_ * 2);
    encoder_ctx_->max_b_frames = 0;
    encoder_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder_ctx_->thread_count = 2;
    //3.2.设置x264 编码器私有参数
    av_opt_set(encoder_ctx_->priv_data, "preset", "ultrafast", 0);
    av_opt_set(encoder_ctx_->priv_data, "tune", "zerolatency", 0);
    // 严格CBR：不使用crf，强制cbr模式，VBV缓冲区约束码率波动
    av_opt_set(encoder_ctx_->priv_data, "cbr", "1", 0);
    av_opt_set(encoder_ctx_->priv_data, "repeat_headers", "1", 0);
    //4.打开编码器
    if (avcodec_open2(encoder_ctx_, codec, nullptr) < 0) {
        SPDLOG_ERROR("Cannot open encoder");
        return false;
    }

    //5.初始化H.264 码流格式转换器（BSF）
    /*
    编码器编码得到的是MP4(AVCC)格式，而RTSP/RTP需要Annex-B格式。
    AVCC和Annex-B都是H.264视频流的编码格式，只是 NAL 单元的封装方式不同 。
    */
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

    SPDLOG_INFO("H264 encoder initialized (ultrafast/zerolatency)");
    return true;
}

bool CameraCapture::init_osd_filter() {
    osd_textfile_path_ = "/tmp/smartcam_osd.txt";
    {
        std::ofstream ofs(osd_textfile_path_);
        ofs << "Initializing..." << std::endl;
    }
    //1.创建滤镜图
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

    //2.创建源滤镜和汇点滤镜并加入滤镜图
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

    //3.创建drawtext滤镜并加入滤镜图
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
    //4.链接滤镜
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
    //5.配置滤镜图
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

    if (!video_source_.start()) {
        SPDLOG_ERROR("Failed to start video file source");
        return;
    }

    running_ = true;
    std::thread(&CameraCapture::capture_loop, this).detach();
    SPDLOG_INFO("CameraCapture started");
}

void CameraCapture::stop() {
    running_ = false;
    video_source_.stop();
    SPDLOG_INFO("CameraCapture stopped");
}

void CameraCapture::capture_loop() {
    RawFrame raw_frame;
    int64_t frame_count = 0;
    uint32_t frame_seq = 0;

    while (running_) {
        if (!video_source_.get_frame(raw_frame, 100)) {
            continue;
        }

        yuv_frame_->data[0] = raw_frame.data.data();
        yuv_frame_->data[1] = raw_frame.data.data() + width_ * height_;
        yuv_frame_->data[2] = raw_frame.data.data() + width_ * height_ * 5 / 4;
        yuv_frame_->linesize[0] = width_;
        yuv_frame_->linesize[1] = width_ / 2;
        yuv_frame_->linesize[2] = width_ / 2;
        yuv_frame_->pts = frame_count++;

        uint64_t t_decode = raw_frame.t_decode;
        uint32_t current_seq = ++frame_seq;

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
            filter_frame_->opaque = reinterpret_cast<void*>(static_cast<uintptr_t>(t_decode));
            encode_frame(filter_frame_, t_decode, current_seq);
            av_frame_unref(filter_frame_);
        } else {
            yuv_frame_->opaque = reinterpret_cast<void*>(static_cast<uintptr_t>(t_decode));
            encode_frame(yuv_frame_, t_decode, current_seq);
        }
    }
}

bool CameraCapture::encode_frame(AVFrame* frame, uint64_t t_decode, uint32_t frame_seq) {
    uint64_t t_encode_in = wall_clock_us();

    uint32_t bitrate = target_bitrate_.load();
    if (encoder_ctx_->bit_rate != static_cast<int64_t>(bitrate) * 1000) {
        encoder_ctx_->bit_rate = static_cast<int64_t>(bitrate) * 1000;
        encoder_ctx_->rc_max_rate = static_cast<int64_t>(bitrate) * 1000;
        encoder_ctx_->rc_buffer_size = static_cast<int64_t>(bitrate) * 1000 / fps_;
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
        output.timestamp = static_cast<uint64_t>(send_pkt->pts * av_q2d(encoder_ctx_->time_base) * 1000000);
        output.data.assign(send_pkt->data, send_pkt->data + send_pkt->size);
        output.is_keyframe = (send_pkt->flags & AV_PKT_FLAG_KEY) != 0;
        output.t_decode = t_decode;
        output.t_encode_in = t_encode_in;
        output.t_encode_out = wall_clock_us();
        output.frame_seq = frame_seq;

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

        // 使用 shared_ptr 分发帧，避免多客户端拷贝
        auto shared_frame = std::make_shared<Frame>(std::move(output));
        shared_frame->t_queue_push = wall_clock_us();

        {
            std::lock_guard<std::mutex> lock(queues_mutex_);
            for (auto& queue : client_queues_) {
                // ---- Reset & Sync 策略 ----
                // 拥塞后清空队列，等待下一个 IDR 帧重新同步

                if (queue->is_waiting_for_sync()) {
                    // 同步等待态：丢弃所有帧，直到遇到 IDR 帧
                    if (shared_frame->is_keyframe) {
                        queue->clear();
                        queue->push(shared_frame);
                        queue->set_waiting_for_sync(false);
                        SPDLOG_INFO("Client synced with IDR frame, resuming");
                    }
                    continue;
                }

                // 检测队列积压超过阈值（容量一半），主动触发同步
                if (queue->size_approx() > queue->capacity() / 2) {
                    queue->clear();
                    queue->set_waiting_for_sync(true);
                    SPDLOG_WARN("Queue congestion detected (backlog={}), entering sync wait state",
                                queue->size_approx());
                    // 若当前帧恰好是 IDR，立即恢复
                    if (shared_frame->is_keyframe) {
                        queue->push(shared_frame);
                        queue->set_waiting_for_sync(false);
                        SPDLOG_INFO("Client synced with IDR frame immediately");
                    }
                    continue;
                }

                // 正常推流
                if (!queue->push(shared_frame)) {
                    // push 失败（队列满），进入同步等待态
                    queue->clear();
                    queue->set_waiting_for_sync(true);
                    SPDLOG_WARN("Queue push failed, entering sync wait state");
                }
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

void CameraCapture::add_client_queue(std::shared_ptr<MessageQueue<std::shared_ptr<Frame>>> queue) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    client_queues_.push_back(queue);
    SPDLOG_INFO("Client queue added, total queues: {}", client_queues_.size());
}

void CameraCapture::remove_client_queue(std::shared_ptr<MessageQueue<std::shared_ptr<Frame>>> queue) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = std::remove(client_queues_.begin(), client_queues_.end(), queue);
    if (it != client_queues_.end()) {
        client_queues_.erase(it, client_queues_.end());
        SPDLOG_INFO("Client queue removed, remaining queues: {}", client_queues_.size());
    }
}

} // namespace smartcam
