#ifndef SMARTCAM_MODULES_RTSP_SERVER_H
#define SMARTCAM_MODULES_RTSP_SERVER_H

#include "common/config.h"
#include "common/types.h"
#include "middleware/latest_value.h"
#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <functional>
#include <mutex>

namespace smartcam {

class CameraCapture;
class AudioCapture;

struct ClientSession {
    int fd = -1;
    std::string client_ip;
    std::string session_id;

    int client_rtp_port = 0;
    int client_rtcp_port = 0;
    int server_rtp_port = 0;
    int server_rtcp_port = 0;
    int rtp_sock = -1;
    bool tcp_interleaved = false;
    int rtp_channel = 0;
    int rtcp_channel = 1;
    std::atomic<bool> client_playing{false};
    std::atomic<bool> rtp_running{false};
    std::thread rtp_thread;
    std::mutex fd_mutex;
    std::shared_ptr<LatestValue<Frame>> frame_slot;
    std::atomic<bool> fd_closed{false};
    std::atomic<bool> send_failed{false};

    int audio_client_rtp_port = 0;
    int audio_client_rtcp_port = 0;
    int audio_server_rtp_port = 0;
    int audio_server_rtcp_port = 0;
    int audio_rtp_sock = -1;
    int audio_rtp_channel = 2;
    int audio_rtcp_channel = 3;
    std::atomic<bool> audio_rtp_running{false};
    std::thread audio_rtp_thread;
    std::shared_ptr<LatestValue<AudioFrame>> audio_frame_slot;
    bool audio_setup_done = false;
};

class RtspServer {
public:
    RtspServer() = default;
    ~RtspServer();

    bool init(const StreamingConfig& config);
    void start();
    void stop();

    std::string get_url() const;
    bool is_running() const { return running_; }

    void set_camera(std::shared_ptr<CameraCapture> camera);
    void set_audio(std::shared_ptr<AudioCapture> audio);

private:
    void accept_loop();
    void handle_client(int fd, const std::string& client_ip);

    void handle_options(ClientSession& sess, const std::string& cseq);
    void handle_describe(ClientSession& sess, const std::string& cseq);
    void handle_setup(ClientSession& sess, const std::string& cseq, const std::string& request);
    void handle_play(ClientSession& sess, const std::string& cseq);
    void handle_teardown(ClientSession& sess, const std::string& cseq);
    void handle_get_parameter(ClientSession& sess, const std::string& cseq);

    bool parse_next_request(std::string& recv_buf, std::string& out_request);
    void send_response(ClientSession& sess, int code, const std::string& cseq,
                       const std::string& extra_headers, const std::string& body);
    void send_tcp_rtp(ClientSession& sess, int channel, const uint8_t* data, size_t len);

    void send_rtp_stream(int rtp_sock, const std::string& client_ip, int client_rtp_port,
        std::atomic<bool>& client_playing, std::atomic<bool>& rtp_running,
        std::shared_ptr<LatestValue<Frame>> frame_slot);
    void send_rtp_stream_tcp(int fd, int channel,
        std::function<void(int, const uint8_t*, size_t)> send_fn,
        std::atomic<bool>& client_playing, std::atomic<bool>& rtp_running,
        std::shared_ptr<LatestValue<Frame>> frame_slot);

    void send_audio_rtp_stream(int rtp_sock, const std::string& client_ip, int client_rtp_port,
        std::atomic<bool>& client_playing, std::atomic<bool>& audio_rtp_running,
        std::shared_ptr<LatestValue<AudioFrame>> audio_frame_slot);
    void send_audio_rtp_stream_tcp(int fd, int rtp_channel, int rtcp_channel,
        std::function<void(int, const uint8_t*, size_t)> send_fn,
        std::atomic<bool>& client_playing, std::atomic<bool>& audio_rtp_running,
        std::shared_ptr<LatestValue<AudioFrame>> audio_frame_slot);

    void send_rtcp_sr_tcp(std::function<void(int, const uint8_t*, size_t)> send_fn,
        int rtcp_channel, uint32_t ssrc, uint64_t ntp_timestamp, uint32_t rtp_timestamp,
        uint32_t packet_count, uint32_t octet_count);

    std::string get_local_ip() const;
    void extract_sps_pps(const uint8_t* data, size_t size);
    std::string get_sprop_parameter_sets() const;

    std::shared_ptr<CameraCapture> camera_;
    std::shared_ptr<AudioCapture> audio_;
    StreamingConfig config_;
    std::string url_;

    int server_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    mutable std::mutex sps_mutex_;
    std::vector<uint8_t> sps_data_;
    std::vector<uint8_t> pps_data_;
    bool sps_cached_ = false;
    bool pps_cached_ = false;
};

} // namespace smartcam

#endif // SMARTCAM_MODULES_RTSP_SERVER_H
