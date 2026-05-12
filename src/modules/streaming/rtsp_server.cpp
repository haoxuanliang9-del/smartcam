#include "rtsp_server.h"
#include <spdlog/spdlog.h>
#include <thread>

#include <liveMedia.hh>
#include <BasicUsageEnvironment.hh>
#include <GroupsockHelper.hh>

namespace smartcam {

class H264FramedSource : public FramedSource {
public:
    static H264FramedSource* createNew(UsageEnvironment& env,
                                       std::shared_ptr<MessageQueue<Frame>> queue) {
        return new H264FramedSource(env, std::move(queue));
    }

protected:
    H264FramedSource(UsageEnvironment& env,
                     std::shared_ptr<MessageQueue<Frame>> queue)
        : FramedSource(env), frame_queue_(std::move(queue)) {}

    void doGetNextFrame() override {
        Frame frame;
        if (frame_queue_->pop(frame, 500)) {
            if (frame.size > fMaxSize) {
                fNumTruncatedBytes = static_cast<unsigned>(frame.size - fMaxSize);
            } else {
                fNumTruncatedBytes = 0;
            }

            memcpy(fTo, frame.data, fMaxSize - fNumTruncatedBytes);
            fFrameSize = static_cast<unsigned>(fMaxSize - fNumTruncatedBytes);

            struct timeval tv;
            tv.tv_sec = static_cast<time_t>(frame.timestamp / 1000000);
            tv.tv_usec = static_cast<suseconds_t>(frame.timestamp % 1000000);
            fPresentationTime = tv;
        } else {
            fFrameSize = 0;
            envir().taskScheduler().scheduleDelayedTask(10000,
                (TaskFunc*)afterGetting, this);
            return;
        }

        afterGetting(this);
    }

private:
    std::shared_ptr<MessageQueue<Frame>> frame_queue_;
};

class H264VideoStreamDiscreteFramer : public H264VideoStreamFramer {
public:
    static H264VideoStreamDiscreteFramer* createNew(UsageEnvironment& env,
                                                     FramedSource* inputSource) {
        return new H264VideoStreamDiscreteFramer(env, inputSource);
    }

protected:
    H264VideoStreamDiscreteFramer(UsageEnvironment& env, FramedSource* inputSource)
        : H264VideoStreamFramer(env, inputSource, False, False) {}
};

RtspServer::RtspServer(std::shared_ptr<MessageQueue<Frame>> frame_queue)
    : frame_queue_(std::move(frame_queue)) {}

RtspServer::~RtspServer() {
    stop();
}

bool RtspServer::init(const StreamingConfig& config, uint32_t width, uint32_t height, uint32_t fps) {
    config_ = config;
    width_ = width;
    height_ = height;
    fps_ = fps;

    SPDLOG_INFO("RTSP server initialized: rtsp://0.0.0.0:{}/{}",
                config.rtsp_port, config.stream_name);
    return true;
}

void RtspServer::start() {
    if (running_) return;
    running_ = true;
    std::thread(&RtspServer::server_loop, this).detach();
    SPDLOG_INFO("RTSP server started on port {}", config_.rtsp_port);
}

void RtspServer::stop() {
    running_ = false;

    if (live_server_) {
        auto* rtsp = static_cast<RTSPServer*>(live_server_);
        Medium::close(rtsp);
        live_server_ = nullptr;
    }

    SPDLOG_INFO("RTSP server stopped");
}

void RtspServer::server_loop() {
    TaskScheduler* scheduler = BasicTaskScheduler::createNew();
    UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

    UserAuthenticationDatabase* authDB = nullptr;

    RTSPServer* rtspServer = RTSPServer::createNew(*env, config_.rtsp_port, authDB);
    if (rtspServer == nullptr) {
        SPDLOG_ERROR("Failed to create RTSP server: {}", env->getResultMsg());
        env->reclaim();
        delete scheduler;
        running_ = false;
        return;
    }

    live_server_ = rtspServer;

    ServerMediaSession* sms = ServerMediaSession::createNew(*env, config_.stream_name.c_str(),
        config_.stream_name.c_str(), "SmartCam H.264 Live Stream");

    auto* source = H264FramedSource::createNew(*env, frame_queue_);
    auto* framer = H264VideoStreamDiscreteFramer::createNew(*env, source);

    sms->addSubsession(PassiveServerMediaSubsession::createNew(*framer, rtspServer->rtspAddress()));

    rtspServer->addServerMediaSession(sms);

    char* url = rtspServer->rtspURL(sms);
    SPDLOG_INFO("RTSP stream available at {}", url);
    delete[] url;

    env->taskScheduler().doEventLoop(reinterpret_cast<char*>(0));

    env->reclaim();
    delete scheduler;
}

std::string RtspServer::get_url() const {
    return "rtsp://0.0.0.0:" + std::to_string(config_.rtsp_port) + "/" + config_.stream_name;
}

} // namespace smartcam
