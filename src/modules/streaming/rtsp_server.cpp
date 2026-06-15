#include "rtsp_server.h"
#include "modules/camera/camera_capture.h"
#include "modules/audio/audio_capture.h"
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <thread>
#include <chrono>
#include <fcntl.h>
#include <algorithm>
#include <random>
#include <sys/select.h>
#include <ctime>
#include <fstream>
#include <time.h>

namespace smartcam {

static uint64_t wall_clock_us() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + ts.tv_nsec / 1000ULL;
}

class LatencyLogger {
public:
    static LatencyLogger& instance() {
        static LatencyLogger logger;
        return logger;
    }

    void open(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ofs_.is_open()) return;
        ofs_.open(path, std::ios::out | std::ios::trunc);
        ofs_ << "frame_seq,is_keyframe,t_decode,t_encode_in,t_encode_out,t_queue_push,t_rtp_send,"
             << "encode_us,queue_us,server_us" << "\n";
        ofs_.flush();
    }

    void log(const Frame& frame) {
        uint64_t encode_us = frame.t_encode_out - frame.t_encode_in;
        uint64_t queue_us = frame.t_rtp_send - frame.t_queue_push;
        uint64_t server_us = frame.t_rtp_send - frame.t_decode;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ofs_.is_open()) return;
        ofs_ << frame.frame_seq << ","
             << (frame.is_keyframe ? 1 : 0) << ","
             << frame.t_decode << ","
             << frame.t_encode_in << ","
             << frame.t_encode_out << ","
             << frame.t_queue_push << ","
             << frame.t_rtp_send << ","
             << encode_us << ","
             << queue_us << ","
             << server_us << "\n";
        count_++;
        if (count_ % 25 == 0) ofs_.flush();
    }

private:
    LatencyLogger() = default;
    std::ofstream ofs_;
    std::mutex mutex_;
    int count_ = 0;
};

// ---------------------------------------------------------------------------
// 前向声明：RTCP Sender Report 辅助函数
// ---------------------------------------------------------------------------

static uint64_t get_ntp_timestamp();
static std::vector<uint8_t> build_rtcp_sr(uint32_t ssrc, uint64_t ntp_timestamp,
                                           uint32_t rtp_timestamp,
                                           uint32_t packet_count, uint32_t octet_count);

RtspServer::~RtspServer() { stop(); }

void RtspServer::set_camera(std::shared_ptr<CameraCapture> camera) {
    camera_ = camera;
}

void RtspServer::set_audio(std::shared_ptr<AudioCapture> audio) {
    audio_ = audio;
}

//初始化TCP链接
bool RtspServer::init(const StreamingConfig& config) {
    config_ = config;
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        SPDLOG_ERROR("RTSP: socket() failed: {}", strerror(errno));
        return false;
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.rtsp_port);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        SPDLOG_ERROR("RTSP: bind() on port {} failed: {}", config_.rtsp_port, strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 5) < 0) {
        SPDLOG_ERROR("RTSP: listen() failed: {}", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    url_ = "rtsp://" + get_local_ip() + ":" + std::to_string(config_.rtsp_port) + "/" + config_.stream_name;
    SPDLOG_INFO("RTSP server initialized on port {}", config_.rtsp_port);
    LatencyLogger::instance().open("/tmp/smartcam_latency.csv");
    return true;
}

//启动accept链接线程
void RtspServer::start() {
    if (running_) return;
    running_ = true;
    accept_thread_ = std::thread(&RtspServer::accept_loop, this);
    SPDLOG_INFO("RTSP server started: {}", url_);
}

void RtspServer::stop() {
    if (!running_) return;
    running_ = false;
    if (server_fd_ >= 0) { shutdown(server_fd_, SHUT_RDWR); close(server_fd_); server_fd_ = -1; }
    if (accept_thread_.joinable()) accept_thread_.join();
    // 客户端线程已 detach，通过 running_=false 自行退出
    client_threads_.clear();
    SPDLOG_INFO("RTSP server stopped");
}

//获取RTSP服务器URL
std::string RtspServer::get_url() const { return url_; }

//获取本地IP地址
std::string RtspServer::get_local_ip() const {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "0.0.0.0";
    struct sockaddr_in loopback = {};
    loopback.sin_family = AF_INET;
    loopback.sin_addr.s_addr = inet_addr("8.8.8.8");
    loopback.sin_port = htons(53);
    connect(sock, (struct sockaddr*)&loopback, sizeof(loopback));
    char buf[64] = {};
    socklen_t len = sizeof(loopback);
    getsockname(sock, (struct sockaddr*)&loopback, &len);
    close(sock);
    inet_ntop(AF_INET, &loopback.sin_addr.s_addr, buf, sizeof(buf));
    return std::string(buf);
}

//accept链接循环
void RtspServer::accept_loop() {
    while (running_) {
        struct sockaddr_in client_addr = {};
        socklen_t clen = sizeof(client_addr);
        int fd = accept(server_fd_, (struct sockaddr*)&client_addr, &clen);
        if (fd < 0) continue;
        std::string cip = inet_ntoa(client_addr.sin_addr);
        SPDLOG_INFO("RTSP client connected from {}:{}", cip, ntohs(client_addr.sin_port));

        std::thread([this, fd, cip]() {
            handle_client(fd, cip);
        }).detach();
    }
}

//解析指定头字段的值
static std::string extract_header(const std::string& request, const std::string& header) {
    std::string search = header + ":";
    size_t pos = request.find(search);
    if (pos == std::string::npos) return "";
    size_t start = pos + search.size();
    while (start < request.size() && request[start] == ' ') start++;
    size_t end = request.find("\r\n", start);
    if (end == std::string::npos) end = request.size();
    std::string val = request.substr(start, end - start);
    while (!val.empty() && (val.back() == ' ' || val.back() == '\r')) val.pop_back();
    return val;
}

static std::vector<std::pair<const uint8_t*, size_t>> parse_nals(const uint8_t* data, size_t size) {
    std::vector<std::pair<const uint8_t*, size_t>> nals;
    size_t i = 0;
    while (i < size) {
        if (i + 3 < size && data[i]==0 && data[i+1]==0 && data[i+2]==0 && data[i+3]==1) {
            i += 4;
        } else if (i + 2 < size && data[i]==0 && data[i+1]==0 && data[i+2]==1) {
            i += 3;
        } else {
            size_t nal_start = i;
            while (i < size) {
                if (i + 3 < size && data[i]==0 && data[i+1]==0 && data[i+2]==0 && data[i+3]==1) break;
                if (i + 2 < size && data[i]==0 && data[i+1]==0 && data[i+2]==1) break;
                i++;
            }
            if (i > nal_start) {
                nals.emplace_back(data + nal_start, i - nal_start);
            }
        }
    }
    return nals;
}

//把二进制数据转换成文本字符串
static std::string base64_encode(const uint8_t* data, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < len) n |= (uint32_t)data[i+1] << 8;
        if (i + 2 < len) n |= (uint32_t)data[i+2];
        result.push_back(table[(n >> 18) & 0x3F]);
        result.push_back(table[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < len) ? table[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < len) ? table[n & 0x3F] : '=');
    }
    return result;
}

void RtspServer::extract_sps_pps(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(sps_mutex_);
    auto nals = parse_nals(data, size);
    for (auto& nal : nals) {
        if (nal.second < 1) continue;
        uint8_t nal_type = nal.first[0] & 0x1F;
        if (nal_type == 7 && !sps_cached_) {
            sps_data_.assign(nal.first, nal.first + nal.second);
            sps_cached_ = true;
            SPDLOG_INFO("RTSP: cached SPS ({} bytes)", nal.second);
        } else if (nal_type == 8 && !pps_cached_) {
            pps_data_.assign(nal.first, nal.first + nal.second);
            pps_cached_ = true;
            SPDLOG_INFO("RTSP: cached PPS ({} bytes)", nal.second);
        }
    }
}

//获取已经缓存好的H.264 视频编码的关键参数集SPS/PPS
std::string RtspServer::get_sprop_parameter_sets() const {
    std::lock_guard<std::mutex> lock(sps_mutex_);
    if (!sps_cached_ || !pps_cached_) return "";
    return base64_encode(sps_data_.data(), sps_data_.size()) + "," +
           base64_encode(pps_data_.data(), pps_data_.size());
}

// ---------------------------------------------------------------------------
// Request/response helpers
// ---------------------------------------------------------------------------
//解析RTSP请求报文
bool RtspServer::parse_next_request(std::string& recv_buf, std::string& out_request) {
    //如果开头是一个完整交错帧，则删除它
    while (!recv_buf.empty() && recv_buf[0] == '$') {
        if (recv_buf.size() < 4) return false;
        size_t pkt_len = ((uint8_t)recv_buf[2] << 8) | (uint8_t)recv_buf[3];
        if (recv_buf.size() < 4 + pkt_len) return false;
        recv_buf.erase(0, 4 + pkt_len);
    }

    // 交错帧不完整，解析失败
    size_t header_end = recv_buf.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;

    //解析消息体长度
    size_t content_len = 0;
    size_t content_len_pos = recv_buf.find("Content-Length:");
    if (content_len_pos != std::string::npos && content_len_pos < header_end) {
        size_t vstart = recv_buf.find(':', content_len_pos) + 1;
        while (vstart < recv_buf.size() && recv_buf[vstart] == ' ') vstart++;
        content_len = atoi(recv_buf.c_str() + vstart);
        // 防止恶意 Content-Length 导致超大内存分配
        if (content_len > 512 * 1024) {
            SPDLOG_WARN("RTSP: Content-Length {} too large, discarding request", content_len);
            recv_buf.erase(0, header_end + 4);
            return false;
        }
    }

    //消息体不完整，解析失败
    size_t total_len = header_end + 4 + content_len;
    if (recv_buf.size() < total_len) return false;

    //解析成功，返回完整RTSP请求报文
    out_request = recv_buf.substr(0, total_len);
    recv_buf.erase(0, total_len);
    return true;
}

//构造一个RTSP响应报文并发送给客户端
void RtspServer::send_response(ClientSession& sess, int code, const std::string& cseq,
                               const std::string& extra_headers, const std::string& body) {
    std::ostringstream ss;
    ss << "RTSP/1.0 " << code << " ";
    switch (code) {
        case 200: ss << "OK"; break;
        case 400: ss << "Bad Request"; break;
        case 404: ss << "Not Found"; break;
        case 405: ss << "Method Not Allowed"; break;
        case 461: ss << "Unsupported transport"; break;
        default: ss << "OK"; break;
    }
    ss << "\r\n";
    if (!cseq.empty()) ss << "CSeq: " << cseq << "\r\n";
    ss << extra_headers;
    if (!body.empty()) ss << "Content-Length: " << body.size() << "\r\n";
    ss << "\r\n";
    if (!body.empty()) ss << body;
    std::string resp = ss.str();
    SPDLOG_DEBUG("RTSP response:\n{}", resp);
    //fd在RTP发送线程中会被访问，需要上锁
    std::lock_guard<std::mutex> lock(sess.fd_mutex);
    if (sess.fd_closed) return;
    ssize_t ret = ::send(sess.fd, resp.data(), resp.size(), MSG_NOSIGNAL);
    if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        SPDLOG_WARN("RTSP: send_response failed: {}", strerror(errno));
        sess.send_failed = true;
    } else if (ret < (ssize_t)resp.size()) {
        SPDLOG_WARN("RTSP: send_response partial send {}/{}", ret, resp.size());
        sess.send_failed = true;
    }
}

#include <poll.h>

void RtspServer::send_tcp_rtp(ClientSession& sess, int channel, const uint8_t* data, size_t len) {
    if (sess.fd_closed || sess.send_failed) return;

    // 将 header + data 拼成连续 buffer，避免 header 和 data 之间被其他线程插入
    // 这解决了 TCP interleaved 模式下音视频线程交叉发送导致的数据流污染问题
    std::vector<uint8_t> packet(4 + len);
    packet[0] = '$';
    packet[1] = static_cast<uint8_t>(channel);
    packet[2] = (len >> 8) & 0xFF;
    packet[3] = len & 0xFF;
    memcpy(packet.data() + 4, data, len);

    // 音频 channel 使用更短的 poll 超时，避免被视频拥塞拖累（队头阻塞缓解）
    int poll_timeout_ms = (channel % 2 == 1) ? 2 : 10;

    size_t sent = 0;
    while (sent < packet.size() && sess.client_playing && running_ && !sess.fd_closed) {
        ssize_t n = 0;
        {
            std::lock_guard<std::mutex> lock(sess.fd_mutex);
            if (sess.fd_closed) break;
            n = ::send(sess.fd, packet.data() + sent, packet.size() - sent, MSG_NOSIGNAL);
        }
        if (n > 0) {
            sent += n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            pfd.fd = sess.fd;
            pfd.events = POLLOUT;
            int pret = poll(&pfd, 1, poll_timeout_ms);
            // 音频：如果 poll 超时仍不可写，直接丢弃当前包（实时性优先于可靠性）
            if (pret == 0 && channel % 2 == 1) {
                SPDLOG_DEBUG("Audio TCP: send congestion, dropping packet to avoid HOL blocking");
                return;
            }
        } else {
            sess.send_failed = true;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// RTSP method handlers
// ---------------------------------------------------------------------------

//告知客户端支持的RTSP方法
//包含请求行：Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER
void RtspServer::handle_options(ClientSession& sess, const std::string& cseq) {
    send_response(sess, 200, cseq,
        "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n", "");
}

//告知媒体信息
//需要发送一个SDP包（双轨道：视频+音频）
void RtspServer::handle_describe(ClientSession& sess, const std::string& cseq) {
    std::string sprop = get_sprop_parameter_sets();
    std::string fmtp = "a=fmtp:96 packetization-mode=1";
    if (!sprop.empty()) {
        fmtp += ";sprop-parameter-sets=" + sprop;
    }
    fmtp += "\r\n";

    std::string sdp =
        "v=0\r\n"
        "o=- 0 0 IN IP4 " + get_local_ip() + "\r\n"
        "s=SmartCam\r\n"
        "c=IN IP4 " + get_local_ip() + "\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=control:trackID=0\r\n" +
        fmtp +
        "m=audio 0 RTP/AVP 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=control:trackID=1\r\n";
    send_response(sess, 200, cseq,
        "Content-Type: application/sdp\r\n", sdp);
}

/*
SETUP请求的核心字段是Transport头字段，一般格式为：
Transport: 传输协议;传输模式;客户端端口（一对，一个RTP一个RTCP）;其它参数
支持视频(trackID=0)和音频(trackID=1)分别SETUP
*/
void RtspServer::handle_setup(ClientSession& sess, const std::string& cseq,
                              const std::string& request) {
    std::string transport_hdr = extract_header(request, "Transport");
    SPDLOG_INFO("RTSP SETUP Transport: {}", transport_hdr);

    //判断是视频还是音频SETUP
    bool is_audio_setup = (request.find("trackID=1") != std::string::npos ||
                           request.find("trackid=1") != std::string::npos);

    //检查是否为TCP交错传输
    bool tcp_interleaved = (transport_hdr.find("interleaved") != std::string::npos);

    if (is_audio_setup) {
        // ===== 音频 SETUP =====
        if (tcp_interleaved) {
            size_t il_pos = transport_hdr.find("interleaved=");
            if (il_pos != std::string::npos) {
                size_t start = il_pos + 12;
                size_t dash = transport_hdr.find('-', start);
                sess.audio_rtp_channel = atoi(transport_hdr.substr(start, dash - start).c_str());
                sess.audio_rtcp_channel = atoi(transport_hdr.substr(dash + 1, 1).c_str());
            }
            std::ostringstream transport_resp;
            transport_resp << "Transport: RTP/AVP/TCP;unicast;interleaved="
                          << sess.audio_rtp_channel << "-" << sess.audio_rtcp_channel
                          << "\r\nSession: " << sess.session_id << "\r\n";
            send_response(sess, 200, cseq, transport_resp.str(), "");
            SPDLOG_INFO("RTSP SETUP audio TCP interleaved channels={}-{}", sess.audio_rtp_channel, sess.audio_rtcp_channel);
        } else {
            size_t cp_pos = transport_hdr.find("client_port=");
            if (cp_pos != std::string::npos) {
                size_t start = cp_pos + 12;
                size_t dash = transport_hdr.find('-', start);
                size_t semi = transport_hdr.find(';', start);
                size_t end = (dash != std::string::npos) ? dash :
                             (semi != std::string::npos) ? semi : transport_hdr.size();
                sess.audio_client_rtp_port = atoi(transport_hdr.substr(start, end - start).c_str());

                if (dash != std::string::npos) {
                    start = dash + 1;
                    semi = transport_hdr.find(';', start);
                    end = (semi != std::string::npos) ? semi : transport_hdr.size();
                    sess.audio_client_rtcp_port = atoi(transport_hdr.substr(start, end - start).c_str());
                } else {
                    sess.audio_client_rtcp_port = sess.audio_client_rtp_port + 1;
                }

                //音频RTP的UDP socket
                if (sess.audio_rtp_sock >= 0) close(sess.audio_rtp_sock);
                sess.audio_rtp_sock = socket(AF_INET, SOCK_DGRAM, 0);
                int bsize = 512*1024;
                setsockopt(sess.audio_rtp_sock, SOL_SOCKET, SO_SNDBUF, &bsize, sizeof(bsize));
                struct sockaddr_in srv_addr = {};
                srv_addr.sin_family = AF_INET;
                srv_addr.sin_addr.s_addr = INADDR_ANY;
                srv_addr.sin_port = htons(0);
                if (bind(sess.audio_rtp_sock, (struct sockaddr*)&srv_addr, sizeof(srv_addr)) < 0) {
                    SPDLOG_ERROR("RTSP: failed to bind audio RTP socket: {}", strerror(errno));
                    send_response(sess, 500, cseq, "", "");
                    return;
                }

                socklen_t alen = sizeof(srv_addr);
                getsockname(sess.audio_rtp_sock, (struct sockaddr*)&srv_addr, &alen);
                sess.audio_server_rtp_port = ntohs(srv_addr.sin_port);
                sess.audio_server_rtcp_port = sess.audio_server_rtp_port + 1;

                std::ostringstream transport_resp;
                transport_resp << "Transport: RTP/AVP;unicast;client_port="
                              << sess.audio_client_rtp_port << "-" << sess.audio_client_rtcp_port
                              << ";server_port=" << sess.audio_server_rtp_port << "-" << sess.audio_server_rtcp_port
                              << "\r\nSession: " << sess.session_id << "\r\n";
                send_response(sess, 200, cseq, transport_resp.str(), "");
                SPDLOG_INFO("RTSP SETUP audio UDP client_port={}-{} server_port={}-{}",
                            sess.audio_client_rtp_port, sess.audio_client_rtcp_port,
                            sess.audio_server_rtp_port, sess.audio_server_rtcp_port);
            } else {
                send_response(sess, 461, cseq, "", "");
                return;
            }
        }
        sess.audio_setup_done = true;
        sess.tcp_interleaved = tcp_interleaved;
    } else {
        // ===== 视频 SETUP =====
        sess.tcp_interleaved = tcp_interleaved;
        if (sess.tcp_interleaved) {
            size_t il_pos = transport_hdr.find("interleaved=");
            if (il_pos != std::string::npos) {
                size_t start = il_pos + 12;
                size_t dash = transport_hdr.find('-', start);
                sess.rtp_channel = atoi(transport_hdr.substr(start, dash - start).c_str());
                sess.rtcp_channel = atoi(transport_hdr.substr(dash + 1, 1).c_str());
            }
            std::ostringstream transport_resp;
            transport_resp << "Transport: RTP/AVP/TCP;unicast;interleaved="
                          << sess.rtp_channel << "-" << sess.rtcp_channel
                          << "\r\nSession: " << sess.session_id << "\r\n";
            send_response(sess, 200, cseq, transport_resp.str(), "");
            SPDLOG_INFO("RTSP SETUP video TCP interleaved channels={}-{}", sess.rtp_channel, sess.rtcp_channel);
        } else {
            size_t cp_pos = transport_hdr.find("client_port=");
            if (cp_pos != std::string::npos) {
                size_t start = cp_pos + 12;
                size_t dash = transport_hdr.find('-', start);
                size_t semi = transport_hdr.find(';', start);
                size_t end = (dash != std::string::npos) ? dash :
                             (semi != std::string::npos) ? semi : transport_hdr.size();
                sess.client_rtp_port = atoi(transport_hdr.substr(start, end - start).c_str());

                if (dash != std::string::npos) {
                    start = dash + 1;
                    semi = transport_hdr.find(';', start);
                    end = (semi != std::string::npos) ? semi : transport_hdr.size();
                    sess.client_rtcp_port = atoi(transport_hdr.substr(start, end - start).c_str());
                } else {
                    sess.client_rtcp_port = sess.client_rtp_port + 1;
                }

                if (sess.rtp_sock >= 0) close(sess.rtp_sock);
                sess.rtp_sock = socket(AF_INET, SOCK_DGRAM, 0);
                int bsize = 512*1024;
                setsockopt(sess.rtp_sock, SOL_SOCKET, SO_SNDBUF, &bsize, sizeof(bsize));
                struct sockaddr_in srv_addr = {};
                srv_addr.sin_family = AF_INET;
                srv_addr.sin_addr.s_addr = INADDR_ANY;
                srv_addr.sin_port = htons(0);
                if (bind(sess.rtp_sock, (struct sockaddr*)&srv_addr, sizeof(srv_addr)) < 0) {
                    SPDLOG_ERROR("RTSP: failed to bind RTP socket: {}", strerror(errno));
                    send_response(sess, 500, cseq, "", "");
                    return;
                }

                socklen_t alen = sizeof(srv_addr);
                getsockname(sess.rtp_sock, (struct sockaddr*)&srv_addr, &alen);
                sess.server_rtp_port = ntohs(srv_addr.sin_port);
                sess.server_rtcp_port = sess.server_rtp_port + 1;

                std::ostringstream transport_resp;
                transport_resp << "Transport: RTP/AVP;unicast;client_port="
                              << sess.client_rtp_port << "-" << sess.client_rtcp_port
                              << ";server_port=" << sess.server_rtp_port << "-" << sess.server_rtcp_port
                              << "\r\nSession: " << sess.session_id << "\r\n";
                send_response(sess, 200, cseq, transport_resp.str(), "");
                SPDLOG_INFO("RTSP SETUP video UDP client_port={}-{} server_port={}-{}",
                            sess.client_rtp_port, sess.client_rtcp_port,
                            sess.server_rtp_port, sess.server_rtcp_port);
            } else {
                send_response(sess, 461, cseq, "", "");
            }
        }
    }
}

void RtspServer::handle_play(ClientSession& sess, const std::string& cseq) {
    send_response(sess, 200, cseq,
        "Session: " + sess.session_id + "\r\nRange: npt=0.000-\r\n", "");

    if (!sess.playing) {
        sess.playing = true;
        sess.client_playing = true;
        sess.rtp_running = true;

        //视频RTP
        sess.frame_queue = std::make_shared<MessageQueue<std::shared_ptr<Frame>>>(64);
        if (camera_) {
            camera_->add_client_queue(sess.frame_queue);
        }

        if (sess.tcp_interleaved) {
            SPDLOG_INFO("RTSP PLAY: video TCP interleaved to {}", sess.client_ip);
            sess.rtp_thread = std::thread([this, &sess]() {
                auto send_fn = [this, &sess](int ch, const uint8_t* d, size_t l) {
                    send_tcp_rtp(sess, ch, d, l);
                };
                send_rtp_stream_tcp(sess.fd, sess.rtp_channel, send_fn,
                                    sess.client_playing, sess.rtp_running,
                                    sess.frame_queue);
            });
        } else if (sess.rtp_sock >= 0) {
            SPDLOG_INFO("RTSP PLAY: video UDP streaming to {}:{}", sess.client_ip, sess.client_rtp_port);
            sess.rtp_thread = std::thread([this, &sess]() {
                send_rtp_stream(sess.rtp_sock, sess.client_ip, sess.client_rtp_port,
                                sess.client_playing, sess.rtp_running,
                                sess.frame_queue);
            });
        }

        //音频RTP
        if (audio_ && sess.audio_setup_done) {
            sess.audio_frame_queue = std::make_shared<MessageQueue<std::shared_ptr<AudioFrame>>>(64);
            audio_->add_client_queue(sess.audio_frame_queue);
            sess.audio_rtp_running = true;

            if (sess.tcp_interleaved) {
                SPDLOG_INFO("RTSP PLAY: audio TCP interleaved to {}", sess.client_ip);
                sess.audio_rtp_thread = std::thread([this, &sess]() {
                    auto send_fn = [this, &sess](int ch, const uint8_t* d, size_t l) {
                        send_tcp_rtp(sess, ch, d, l);
                    };
                    send_audio_rtp_stream_tcp(sess.fd, sess.audio_rtp_channel,
                        sess.audio_rtcp_channel, send_fn,
                        sess.client_playing, sess.audio_rtp_running,
                        sess.audio_frame_queue);
                });
            } else if (sess.audio_rtp_sock >= 0) {
                SPDLOG_INFO("RTSP PLAY: audio UDP streaming to {}:{}", sess.client_ip, sess.audio_client_rtp_port);
                sess.audio_rtp_thread = std::thread([this, &sess]() {
                    send_audio_rtp_stream(sess.audio_rtp_sock, sess.client_ip,
                        sess.audio_client_rtp_port,
                        sess.client_playing, sess.audio_rtp_running,
                        sess.audio_frame_queue);
                });
            }
        }
    }
}

void RtspServer::handle_teardown(ClientSession& sess, const std::string& cseq) {
    sess.client_playing = false;
    sess.rtp_running = false;
    sess.audio_rtp_running = false;
    sess.playing = false;

    if (sess.frame_queue && camera_) {
        camera_->remove_client_queue(sess.frame_queue);
    }
    if (sess.audio_frame_queue && audio_) {
        audio_->remove_client_queue(sess.audio_frame_queue);
    }

    send_response(sess, 200, cseq, "Session: " + sess.session_id + "\r\n", "");
}

void RtspServer::handle_get_parameter(ClientSession& sess, const std::string& cseq) {
    send_response(sess, 200, cseq, "Session: " + sess.session_id + "\r\n", "");
}

// ---------------------------------------------------------------------------
// Main client handler
// ---------------------------------------------------------------------------

//处理客户端RTSP请求
void RtspServer::handle_client(int fd, const std::string& client_ip) {
    ClientSession sess;
    sess.fd = fd;
    sess.client_ip = client_ip;

    //生成一个随机数作为会话ID
    std::random_device rd;
    sess.session_id = std::to_string(rd());

    //设置socket为非阻塞模式
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    char buf[65536];
    std::string recv_buf;
    static constexpr size_t MAX_RECV_BUF = 1 * 1024 * 1024;  // 接收缓冲区上限 1MB
    static constexpr size_t MAX_CONTENT_LEN = 512 * 1024;     // Content-Length 上限 512KB
    auto last_activity_time = std::chrono::steady_clock::now();

    while (running_ && !sess.send_failed) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);
        struct timeval tv = {1, 0};
        int sel = select(fd + 1, &read_fds, nullptr, nullptr, &tv);

        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (FD_ISSET(fd, &read_fds)) {
            ssize_t n = recv(fd, buf, sizeof(buf)-1, 0);
            if (n <= 0) {
                if (n == 0) break;
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                break;
            }
            recv_buf.append(buf, n);
            last_activity_time = std::chrono::steady_clock::now();

            // 防止恶意客户端导致 OOM
            if (recv_buf.size() > MAX_RECV_BUF) {
                SPDLOG_WARN("RTSP: recv_buf exceeded {}MB limit, closing connection", MAX_RECV_BUF / 1024 / 1024);
                goto cleanup;
            }
        }

        // 所有阶段都检测超时，防止连接挂起
        auto now = std::chrono::steady_clock::now();
        auto idle_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - last_activity_time).count();
        if (sess.client_playing && idle_seconds > 60) {
            SPDLOG_WARN("RTSP client disconnected due to timeout (no heartbeat for {}s)", idle_seconds);
            goto cleanup;
        } else if (!sess.client_playing && idle_seconds > 30) {
            SPDLOG_WARN("RTSP client disconnected due to pre-PLAY timeout ({}s)", idle_seconds);
            goto cleanup;
        }

        std::string request;
        while (parse_next_request(recv_buf, request)) {
            std::istringstream iss(request);
            std::string method, url, version;
            iss >> method >> url >> version;

            std::string cseq = extract_header(request, "CSeq");
            SPDLOG_INFO("RTSP {} {} CSeq={}", method, url, cseq);

            if (method == "OPTIONS") {
                handle_options(sess, cseq);
            } else if (method == "DESCRIBE") {
                handle_describe(sess, cseq);
            } else if (method == "SETUP") {
                handle_setup(sess, cseq, request);
            } else if (method == "PLAY") {
                handle_play(sess, cseq);
            } else if (method == "TEARDOWN") {
                handle_teardown(sess, cseq);
                goto cleanup;
            } else if (method == "GET_PARAMETER") {
                handle_get_parameter(sess, cseq);
            } else {
                send_response(sess, 405, cseq,
                    "Allow: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n", "");
            }
        }
    }

cleanup:
    sess.client_playing = false;
    sess.rtp_running = false;
    sess.audio_rtp_running = false;
    if (sess.rtp_thread.joinable()) sess.rtp_thread.join();
    if (sess.audio_rtp_thread.joinable()) sess.audio_rtp_thread.join();

    if (sess.frame_queue && camera_) {
        camera_->remove_client_queue(sess.frame_queue);
    }
    if (sess.audio_frame_queue && audio_) {
        audio_->remove_client_queue(sess.audio_frame_queue);
    }

    // 安全关闭 fd：先标记，再加锁关闭，防止 RTP 线程在 close 后使用 fd
    sess.fd_closed = true;
    {
        std::lock_guard<std::mutex> lock(sess.fd_mutex);
        if (sess.rtp_sock >= 0) { close(sess.rtp_sock); sess.rtp_sock = -1; }
        if (sess.audio_rtp_sock >= 0) { close(sess.audio_rtp_sock); sess.audio_rtp_sock = -1; }
        if (sess.fd >= 0) { close(sess.fd); sess.fd = -1; }
    }
    SPDLOG_INFO("RTSP client {} disconnected", client_ip);
}

// ---------------------------------------------------------------------------
// RTP streaming (TCP interleaved)
// ---------------------------------------------------------------------------

void RtspServer::send_rtp_stream_tcp(int fd, int channel,
    std::function<void(int, const uint8_t*, size_t)> send_fn,
    std::atomic<bool>& client_playing, std::atomic<bool>& rtp_running,
    std::shared_ptr<MessageQueue<std::shared_ptr<Frame>>> frame_queue) {

    uint16_t seq = 0;
    uint32_t timestamp = 0;
    std::random_device rd;
    uint32_t ssrc = rd();

    uint32_t packet_count = 0;
    uint32_t octet_count = 0;
    auto last_sr_time = std::chrono::steady_clock::now();

    auto send_nal = [&](const uint8_t* nal_data, size_t nal_size, bool marker) {
        const size_t max_payload = 1400;

        if (nal_size <= max_payload) {
            uint8_t pkt[1500];
            pkt[0] = 0x80;
            pkt[1] = (marker ? 0x80 : 0x00) | 96;
            pkt[2] = (seq >> 8) & 0xFF;
            pkt[3] = seq & 0xFF;
            pkt[4] = (timestamp >> 24) & 0xFF;
            pkt[5] = (timestamp >> 16) & 0xFF;
            pkt[6] = (timestamp >> 8) & 0xFF;
            pkt[7] = timestamp & 0xFF;
            pkt[8] = (ssrc >> 24) & 0xFF;
            pkt[9] = (ssrc >> 16) & 0xFF;
            pkt[10] = (ssrc >> 8) & 0xFF;
            pkt[11] = ssrc & 0xFF;
            memcpy(pkt + 12, nal_data, nal_size);
            send_fn(channel, pkt, 12 + nal_size);
            seq++;
            packet_count++;
            octet_count += static_cast<uint32_t>(nal_size);
        } else {
            uint8_t nal_type = nal_data[0] & 0x1F;
            uint8_t nal_nri = nal_data[0] & 0x60;

            size_t offset = 1;
            bool first = true;

            while (offset < nal_size) {
                size_t chunk = std::min(max_payload - 2, nal_size - offset);
                bool last_chunk = (offset + chunk >= nal_size);

                uint8_t pkt[1500];
                pkt[0] = 0x80;
                pkt[1] = ((last_chunk && marker) ? 0x80 : 0x00) | 96;
                pkt[2] = (seq >> 8) & 0xFF;
                pkt[3] = seq & 0xFF;
                pkt[4] = (timestamp >> 24) & 0xFF;
                pkt[5] = (timestamp >> 16) & 0xFF;
                pkt[6] = (timestamp >> 8) & 0xFF;
                pkt[7] = timestamp & 0xFF;
                pkt[8] = (ssrc >> 24) & 0xFF;
                pkt[9] = (ssrc >> 16) & 0xFF;
                pkt[10] = (ssrc >> 8) & 0xFF;
                pkt[11] = ssrc & 0xFF;

                pkt[12] = nal_nri | 28;
                pkt[13] = nal_type;
                if (first) pkt[13] |= 0x80;
                if (last_chunk) pkt[13] |= 0x40;

                memcpy(pkt + 14, nal_data + offset, chunk);
                send_fn(channel, pkt, 14 + chunk);

                seq++;
                packet_count++;
                octet_count += static_cast<uint32_t>(chunk);
                offset += chunk;
                first = false;
            }
        }
    };

    SPDLOG_INFO("RTP TCP streaming on channel {}", channel);

    std::shared_ptr<Frame> frame_ptr;
    int frame_count = 0;

    while (running_ && client_playing && rtp_running) {
        bool got = frame_queue->pop_move(frame_ptr, 100);
        if (!got || !frame_ptr || frame_ptr->data.empty()) continue;

        frame_ptr->t_rtp_send = wall_clock_us();

        extract_sps_pps(frame_ptr->data.data(), frame_ptr->data.size());

        auto nals = parse_nals(frame_ptr->data.data(), frame_ptr->data.size());

        for (size_t n = 0; n < nals.size(); n++) {
            bool is_last = (n == nals.size() - 1);
            send_nal(nals[n].first, nals[n].second, is_last);
        }

        timestamp = static_cast<uint32_t>(frame_ptr->timestamp * 90000ULL / 1000000ULL);
        frame_count++;

        LatencyLogger::instance().log(*frame_ptr);

        //每5秒发送一次视频RTCP Sender Report
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_sr_time).count();
        if (elapsed >= 5) {
            uint64_t ntp_ts = get_ntp_timestamp();
            send_rtcp_sr_tcp(send_fn, 1, ssrc, ntp_ts, timestamp, packet_count, octet_count);
            last_sr_time = now;
            SPDLOG_DEBUG("Video RTCP SR sent (TCP): ssrc={}, pkt_count={}, oct_count={}", ssrc, packet_count, octet_count);
        }
    }

    SPDLOG_INFO("RTP TCP streaming stopped after {} frames", frame_count);
}

// ---------------------------------------------------------------------------
// RTP streaming (UDP)
// ---------------------------------------------------------------------------

void RtspServer::send_rtp_stream(int rtp_sock, const std::string& client_ip, int client_rtp_port,
    std::atomic<bool>& client_playing, std::atomic<bool>& rtp_running,
    std::shared_ptr<MessageQueue<std::shared_ptr<Frame>>> frame_queue) {

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(client_rtp_port);
    inet_pton(AF_INET, client_ip.c_str(), &dest.sin_addr);

    struct sockaddr_in rtcp_dest = {};
    rtcp_dest.sin_family = AF_INET;
    rtcp_dest.sin_port = htons(client_rtp_port + 1);
    inet_pton(AF_INET, client_ip.c_str(), &rtcp_dest.sin_addr);

    uint16_t seq = 0;
    uint32_t timestamp = 0;
    std::random_device rd;
    uint32_t ssrc = rd();

    uint32_t packet_count = 0;
    uint32_t octet_count = 0;
    auto last_sr_time = std::chrono::steady_clock::now();
    int udp_send_errors = 0;
    static constexpr int MAX_UDP_ERRORS = 100;  // 连续发送失败上限，超过则断开

    auto send_nal = [&](const uint8_t* nal_data, size_t nal_size, bool marker) {
        const size_t max_payload = 1400;

        if (nal_size <= max_payload) {
            uint8_t pkt[1500];
            pkt[0] = 0x80;
            pkt[1] = (marker ? 0x80 : 0x00) | 96;
            pkt[2] = (seq >> 8) & 0xFF;
            pkt[3] = seq & 0xFF;
            pkt[4] = (timestamp >> 24) & 0xFF;
            pkt[5] = (timestamp >> 16) & 0xFF;
            pkt[6] = (timestamp >> 8) & 0xFF;
            pkt[7] = timestamp & 0xFF;
            pkt[8] = (ssrc >> 24) & 0xFF;
            pkt[9] = (ssrc >> 16) & 0xFF;
            pkt[10] = (ssrc >> 8) & 0xFF;
            pkt[11] = ssrc & 0xFF;
            memcpy(pkt + 12, nal_data, nal_size);
            ssize_t ret = sendto(rtp_sock, pkt, 12 + nal_size, 0,
                   (struct sockaddr*)&dest, sizeof(dest));
            if (ret < 0) {
                udp_send_errors++;
                if (udp_send_errors >= MAX_UDP_ERRORS) {
                    SPDLOG_WARN("Video UDP: {} consecutive send errors, stopping", udp_send_errors);
                    client_playing = false;
                    return;
                }
            } else {
                udp_send_errors = 0;
            }
            seq++;
            packet_count++;
            octet_count += static_cast<uint32_t>(nal_size);
        } else {
            uint8_t nal_type = nal_data[0] & 0x1F;
            uint8_t nal_nri = nal_data[0] & 0x60;

            size_t offset = 1;
            bool first = true;

            while (offset < nal_size) {
                size_t chunk = std::min(max_payload - 2, nal_size - offset);
                bool last_chunk = (offset + chunk >= nal_size);

                uint8_t pkt[1500];
                pkt[0] = 0x80;
                pkt[1] = ((last_chunk && marker) ? 0x80 : 0x00) | 96;
                pkt[2] = (seq >> 8) & 0xFF;
                pkt[3] = seq & 0xFF;
                pkt[4] = (timestamp >> 24) & 0xFF;
                pkt[5] = (timestamp >> 16) & 0xFF;
                pkt[6] = (timestamp >> 8) & 0xFF;
                pkt[7] = timestamp & 0xFF;
                pkt[8] = (ssrc >> 24) & 0xFF;
                pkt[9] = (ssrc >> 16) & 0xFF;
                pkt[10] = (ssrc >> 8) & 0xFF;
                pkt[11] = ssrc & 0xFF;

                pkt[12] = nal_nri | 28;
                pkt[13] = nal_type;
                if (first) pkt[13] |= 0x80;
                if (last_chunk) pkt[13] |= 0x40;

                memcpy(pkt + 14, nal_data + offset, chunk);
                ssize_t ret = sendto(rtp_sock, pkt, 14 + chunk, 0,
                       (struct sockaddr*)&dest, sizeof(dest));
                if (ret < 0) {
                    udp_send_errors++;
                    if (udp_send_errors >= MAX_UDP_ERRORS) {
                        SPDLOG_WARN("Video UDP: {} consecutive send errors (FU-A), stopping", udp_send_errors);
                        client_playing = false;
                        return;
                    }
                } else {
                    udp_send_errors = 0;
                }

                seq++;
                packet_count++;
                octet_count += static_cast<uint32_t>(chunk);
                offset += chunk;
                first = false;
            }
        }
    };

    SPDLOG_INFO("RTP UDP streaming to {}:{}", client_ip, client_rtp_port);

    std::shared_ptr<Frame> frame_ptr;
    int frame_count = 0;

    while (running_ && client_playing && rtp_running) {
        bool got = frame_queue->pop_move(frame_ptr, 100);
        if (!got || !frame_ptr || frame_ptr->data.empty()) continue;

        frame_ptr->t_rtp_send = wall_clock_us();

        extract_sps_pps(frame_ptr->data.data(), frame_ptr->data.size());

        auto nals = parse_nals(frame_ptr->data.data(), frame_ptr->data.size());

        for (size_t n = 0; n < nals.size(); n++) {
            bool is_last = (n == nals.size() - 1);
            send_nal(nals[n].first, nals[n].second, is_last);
        }

        timestamp = static_cast<uint32_t>(frame_ptr->timestamp * 90000ULL / 1000000ULL);
        frame_count++;

        LatencyLogger::instance().log(*frame_ptr);

        //每5秒发送一次视频RTCP Sender Report
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_sr_time).count();
        if (elapsed >= 5) {
            uint64_t ntp_ts = get_ntp_timestamp();
            auto sr_pkt = build_rtcp_sr(ssrc, ntp_ts, timestamp, packet_count, octet_count);
            sendto(rtp_sock, sr_pkt.data(), sr_pkt.size(), 0,
                   (struct sockaddr*)&rtcp_dest, sizeof(rtcp_dest));
            last_sr_time = now;
            SPDLOG_DEBUG("Video RTCP SR sent: ssrc={}, pkt_count={}, oct_count={}", ssrc, packet_count, octet_count);
        }
    }

    SPDLOG_INFO("RTP UDP streaming stopped after {} frames", frame_count);
}

// ---------------------------------------------------------------------------
// RTCP Sender Report
// ---------------------------------------------------------------------------

//获取当前NTP时间戳（秒+小数秒，各32位）
static uint64_t get_ntp_timestamp() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ntp_sec = static_cast<uint64_t>(ts.tv_sec) + 2208988800ULL; // Unix epoch -> NTP epoch
    uint64_t ntp_frac = static_cast<uint64_t>(ts.tv_nsec) * (1ULL << 32) / 1000000000ULL;
    return (ntp_sec << 32) | ntp_frac;
}

//构造RTCP Sender Report包（RFC 3550 Section 6.4.1）
static std::vector<uint8_t> build_rtcp_sr(uint32_t ssrc, uint64_t ntp_timestamp,
                                           uint32_t rtp_timestamp,
                                           uint32_t packet_count, uint32_t octet_count) {
    std::vector<uint8_t> pkt(28);
    // V=2, P=0, RC=0
    pkt[0] = 0x80;
    // PT=200 (SR)
    pkt[1] = 200;
    // length = 6 (28字节 - 4字节头) / 4 = 6
    pkt[2] = 0;
    pkt[3] = 6;
    // SSRC
    pkt[4] = (ssrc >> 24) & 0xFF;
    pkt[5] = (ssrc >> 16) & 0xFF;
    pkt[6] = (ssrc >> 8) & 0xFF;
    pkt[7] = ssrc & 0xFF;
    // NTP timestamp (64-bit)
    pkt[8] = (ntp_timestamp >> 56) & 0xFF;
    pkt[9] = (ntp_timestamp >> 48) & 0xFF;
    pkt[10] = (ntp_timestamp >> 40) & 0xFF;
    pkt[11] = (ntp_timestamp >> 32) & 0xFF;
    pkt[12] = (ntp_timestamp >> 24) & 0xFF;
    pkt[13] = (ntp_timestamp >> 16) & 0xFF;
    pkt[14] = (ntp_timestamp >> 8) & 0xFF;
    pkt[15] = ntp_timestamp & 0xFF;
    // RTP timestamp
    pkt[16] = (rtp_timestamp >> 24) & 0xFF;
    pkt[17] = (rtp_timestamp >> 16) & 0xFF;
    pkt[18] = (rtp_timestamp >> 8) & 0xFF;
    pkt[19] = rtp_timestamp & 0xFF;
    // sender's packet count
    pkt[20] = (packet_count >> 24) & 0xFF;
    pkt[21] = (packet_count >> 16) & 0xFF;
    pkt[22] = (packet_count >> 8) & 0xFF;
    pkt[23] = packet_count & 0xFF;
    // sender's octet count
    pkt[24] = (octet_count >> 24) & 0xFF;
    pkt[25] = (octet_count >> 16) & 0xFF;
    pkt[26] = (octet_count >> 8) & 0xFF;
    pkt[27] = octet_count & 0xFF;
    return pkt;
}

void RtspServer::send_rtcp_sr(int sock, const std::string& client_ip, int client_rtcp_port,
    uint32_t ssrc, uint64_t ntp_timestamp, uint32_t rtp_timestamp,
    uint32_t packet_count, uint32_t octet_count) {
    auto pkt = build_rtcp_sr(ssrc, ntp_timestamp, rtp_timestamp, packet_count, octet_count);
    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(client_rtcp_port);
    inet_pton(AF_INET, client_ip.c_str(), &dest.sin_addr);
    sendto(sock, pkt.data(), pkt.size(), 0, (struct sockaddr*)&dest, sizeof(dest));
}

void RtspServer::send_rtcp_sr_tcp(std::function<void(int, const uint8_t*, size_t)> send_fn,
    int rtcp_channel, uint32_t ssrc, uint64_t ntp_timestamp, uint32_t rtp_timestamp,
    uint32_t packet_count, uint32_t octet_count) {
    auto pkt = build_rtcp_sr(ssrc, ntp_timestamp, rtp_timestamp, packet_count, octet_count);
    send_fn(rtcp_channel, pkt.data(), pkt.size());
}

// ---------------------------------------------------------------------------
// 音频 RTP streaming (UDP)
// ---------------------------------------------------------------------------

void RtspServer::send_audio_rtp_stream(int rtp_sock, const std::string& client_ip, int client_rtp_port,
    std::atomic<bool>& client_playing, std::atomic<bool>& audio_rtp_running,
    std::shared_ptr<MessageQueue<std::shared_ptr<AudioFrame>>> audio_frame_queue) {

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(client_rtp_port);
    inet_pton(AF_INET, client_ip.c_str(), &dest.sin_addr);

    struct sockaddr_in rtcp_dest = {};
    rtcp_dest.sin_family = AF_INET;
    rtcp_dest.sin_port = htons(client_rtp_port + 1);
    inet_pton(AF_INET, client_ip.c_str(), &rtcp_dest.sin_addr);

    uint16_t seq = 0;
    std::random_device rd;
    uint32_t ssrc = rd();

    uint32_t packet_count = 0;
    uint32_t octet_count = 0;
    auto last_sr_time = std::chrono::steady_clock::now();
    uint32_t start_rtp_timestamp = 0;
    bool first_frame = true;
    int udp_send_errors = 0;
    static constexpr int MAX_UDP_ERRORS = 100;

    SPDLOG_INFO("Audio RTP UDP streaming to {}:{}", client_ip, client_rtp_port);

    std::shared_ptr<AudioFrame> frame_ptr;
    int frame_count = 0;

    while (running_ && client_playing && audio_rtp_running) {
        bool got = audio_frame_queue->pop_move(frame_ptr, 100);
        if (!got || !frame_ptr || frame_ptr->data.empty()) continue;

        // 使用帧的绝对采集时间戳作为 RTP 时间戳基准（与视频一致）
        uint32_t frame_rtp_ts = static_cast<uint32_t>(frame_ptr->timestamp * 8 / 1000);
        if (first_frame) {
            start_rtp_timestamp = frame_rtp_ts;
            first_frame = false;
        }

        //按160字节（20ms）切分G.711 PCMU数据
        const size_t max_audio_payload = 160;
        size_t offset = 0;
        uint32_t samples_offset = 0;

        while (offset < frame_ptr->data.size()) {
            size_t chunk = std::min(max_audio_payload, frame_ptr->data.size() - offset);
            bool marker = (offset + chunk >= frame_ptr->data.size());

            uint32_t pkt_ts = frame_rtp_ts + samples_offset;

            uint8_t pkt[200];
            //RTP头部：V=2, P=0, X=0, CC=0
            pkt[0] = 0x80;
            //M=marker, PT=0 (PCMU)
            pkt[1] = (marker ? 0x80 : 0x00) | 0;
            pkt[2] = (seq >> 8) & 0xFF;
            pkt[3] = seq & 0xFF;
            pkt[4] = (pkt_ts >> 24) & 0xFF;
            pkt[5] = (pkt_ts >> 16) & 0xFF;
            pkt[6] = (pkt_ts >> 8) & 0xFF;
            pkt[7] = pkt_ts & 0xFF;
            pkt[8] = (ssrc >> 24) & 0xFF;
            pkt[9] = (ssrc >> 16) & 0xFF;
            pkt[10] = (ssrc >> 8) & 0xFF;
            pkt[11] = ssrc & 0xFF;
            memcpy(pkt + 12, frame_ptr->data.data() + offset, chunk);
            ssize_t ret = sendto(rtp_sock, pkt, 12 + chunk, 0,
                   (struct sockaddr*)&dest, sizeof(dest));
            if (ret < 0) {
                udp_send_errors++;
                if (udp_send_errors >= MAX_UDP_ERRORS) {
                    SPDLOG_WARN("Audio UDP: {} consecutive send errors, stopping", udp_send_errors);
                    client_playing = false;
                    break;
                }
            } else {
                udp_send_errors = 0;
            }

            seq++;
            samples_offset += static_cast<uint32_t>(chunk);
            packet_count++;
            octet_count += static_cast<uint32_t>(chunk);
            offset += chunk;
        }

        frame_count++;

        //每5秒发送一次RTCP Sender Report
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_sr_time).count();
        if (elapsed >= 5) {
            uint64_t ntp_ts = get_ntp_timestamp();
            uint32_t rtp_ts = static_cast<uint32_t>(frame_ptr->timestamp * 8 / 1000);
            //使用RTP socket发送RTCP SR到客户端RTCP端口
            auto sr_pkt = build_rtcp_sr(ssrc, ntp_ts, rtp_ts, packet_count, octet_count);
            sendto(rtp_sock, sr_pkt.data(), sr_pkt.size(), 0,
                   (struct sockaddr*)&rtcp_dest, sizeof(rtcp_dest));
            last_sr_time = now;
            SPDLOG_DEBUG("Audio RTCP SR sent: ssrc={}, pkt_count={}, oct_count={}", ssrc, packet_count, octet_count);
        }
    }

    SPDLOG_INFO("Audio RTP UDP streaming stopped after {} frames", frame_count);
}

// ---------------------------------------------------------------------------
// 音频 RTP streaming (TCP interleaved)
// ---------------------------------------------------------------------------

void RtspServer::send_audio_rtp_stream_tcp(int fd, int rtp_channel, int rtcp_channel,
    std::function<void(int, const uint8_t*, size_t)> send_fn,
    std::atomic<bool>& client_playing, std::atomic<bool>& audio_rtp_running,
    std::shared_ptr<MessageQueue<std::shared_ptr<AudioFrame>>> audio_frame_queue) {

    uint16_t seq = 0;
    std::random_device rd;
    uint32_t ssrc = rd();

    uint32_t packet_count = 0;
    uint32_t octet_count = 0;
    auto last_sr_time = std::chrono::steady_clock::now();
    uint32_t start_rtp_timestamp = 0;
    bool first_frame = true;

    SPDLOG_INFO("Audio RTP TCP streaming on channel {}", rtp_channel);

    std::shared_ptr<AudioFrame> frame_ptr;
    int frame_count = 0;

    while (running_ && client_playing && audio_rtp_running) {
        bool got = audio_frame_queue->pop_move(frame_ptr, 100);
        if (!got || !frame_ptr || frame_ptr->data.empty()) continue;

        // 使用帧的绝对采集时间戳作为 RTP 时间戳基准（与视频一致）
        // 丢帧时时间戳会自然跳变，避免与 RTCP SR 的绝对时间戳撕裂
        uint32_t frame_rtp_ts = static_cast<uint32_t>(frame_ptr->timestamp * 8 / 1000);
        if (first_frame) {
            start_rtp_timestamp = frame_rtp_ts;
            first_frame = false;
        }

        const size_t max_audio_payload = 160;
        size_t offset = 0;
        uint32_t samples_offset = 0;  // 帧内采样偏移

        while (offset < frame_ptr->data.size()) {
            size_t chunk = std::min(max_audio_payload, frame_ptr->data.size() - offset);
            bool marker = (offset + chunk >= frame_ptr->data.size());

            // 每个 RTP 包的时间戳 = 帧基准 + 帧内偏移
            uint32_t pkt_ts = frame_rtp_ts + samples_offset;

            uint8_t pkt[200];
            pkt[0] = 0x80;
            pkt[1] = (marker ? 0x80 : 0x00) | 0;
            pkt[2] = (seq >> 8) & 0xFF;
            pkt[3] = seq & 0xFF;
            pkt[4] = (pkt_ts >> 24) & 0xFF;
            pkt[5] = (pkt_ts >> 16) & 0xFF;
            pkt[6] = (pkt_ts >> 8) & 0xFF;
            pkt[7] = pkt_ts & 0xFF;
            pkt[8] = (ssrc >> 24) & 0xFF;
            pkt[9] = (ssrc >> 16) & 0xFF;
            pkt[10] = (ssrc >> 8) & 0xFF;
            pkt[11] = ssrc & 0xFF;
            memcpy(pkt + 12, frame_ptr->data.data() + offset, chunk);
            send_fn(rtp_channel, pkt, 12 + chunk);

            seq++;
            samples_offset += static_cast<uint32_t>(chunk);
            packet_count++;
            octet_count += static_cast<uint32_t>(chunk);
            offset += chunk;
        }

        frame_count++;

        //每5秒发送一次RTCP Sender Report
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_sr_time).count();
        if (elapsed >= 5) {
            uint64_t ntp_ts = get_ntp_timestamp();
            uint32_t rtp_ts = static_cast<uint32_t>(frame_ptr->timestamp * 8 / 1000);
            send_rtcp_sr_tcp(send_fn, rtcp_channel, ssrc, ntp_ts, rtp_ts, packet_count, octet_count);
            last_sr_time = now;
            SPDLOG_DEBUG("Audio RTCP SR sent (TCP): ssrc={}, pkt_count={}, oct_count={}", ssrc, packet_count, octet_count);
        }
    }

    SPDLOG_INFO("Audio RTP TCP streaming stopped after {} frames", frame_count);
}

} // namespace smartcam
