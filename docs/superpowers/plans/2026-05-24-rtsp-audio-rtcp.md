# RTSP 音频推流 + RTCP 协议 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 为 SmartCam 添加音频采集（G.711 PCMU via ALSA）和 RTCP Sender Report，实现 RTSP 音视频双轨道推流。

**架构：** 单 Session 双媒体流模式 — 一个 RTSP Session 同时包含视频和音频轨道，SDP 声明两个 m= 行，客户端分别 SETUP 视频和音频，PLAY 时同时启动音视频 RTP 线程，RTCP SR 定期发送用于音视频同步。

**技术栈：** C++17, ALSA (libasound), G.711 PCMU 查表编码, RTP/RTCP 协议

---

## 文件结构

| 文件 | 操作 | 职责 |
|------|------|------|
| `src/common/types.h` | 修改 | 新增 AudioFrame 类型 |
| `src/common/config.h` | 修改 | StreamingConfig 新增 audio_device 字段 |
| `src/common/config.cpp` | 修改 | 解析 audio_device 配置 |
| `src/modules/audio/audio_capture.h` | 创建 | AudioCapture 类声明 |
| `src/modules/audio/audio_capture.cpp` | 创建 | ALSA 采集 + G.711 PCMU 编码实现 |
| `src/modules/streaming/rtsp_server.h` | 修改 | ClientSession 扩展音频字段，新增音频方法 |
| `src/modules/streaming/rtsp_server.cpp` | 修改 | SDP 双轨道、音频 SETUP、音频 RTP、RTCP SR |
| `src/app/main_service.h` | 修改 | 新增 AudioCapture 成员 |
| `src/app/main_service.cpp` | 修改 | 初始化和启动音频模块 |
| `CMakeLists.txt` | 修改 | 新增源文件，链接 asound |

---

### 任务 1：新增 AudioFrame 类型和音频配置

**文件：**
- 修改：`src/common/types.h`
- 修改：`src/common/config.h`
- 修改：`src/common/config.cpp`

- [ ] **步骤 1：在 types.h 中新增 AudioFrame**

在 `Frame` 结构体之后添加：

```cpp
struct AudioFrame {
    uint64_t timestamp = 0;        // 微秒
    std::vector<uint8_t> data;     // G.711 PCMU 编码数据
};
```

- [ ] **步骤 2：在 config.h 的 StreamingConfig 中新增 audio_device**

```cpp
struct StreamingConfig {
    uint16_t rtsp_port = 8554;
    std::string stream_name = "live";
    uint32_t max_clients = 10;
    std::string audio_device = "plughw:0,0";
};
```

- [ ] **步骤 3：在 config.cpp 中解析 audio_device**

在 `StreamingConfig` 的解析部分添加 `audio_device` 字段的 YAML 读取。

- [ ] **步骤 4：编译验证**

运行：`cd /home/dministrator/work/smartcam/build && cmake .. && make -j$(nproc)`
预期：编译通过

- [ ] **步骤 5：Commit**

```bash
git add src/common/types.h src/common/config.h src/common/config.cpp
git commit -m "feat: add AudioFrame type and audio_device config"
```

---

### 任务 2：创建 AudioCapture 模块

**文件：**
- 创建：`src/modules/audio/audio_capture.h`
- 创建：`src/modules/audio/audio_capture.cpp`

- [ ] **步骤 1：创建 audio_capture.h**

```cpp
#ifndef SMARTCAM_MODULES_AUDIO_CAPTURE_H
#define SMARTCAM_MODULES_AUDIO_CAPTURE_H

#include "common/config.h"
#include "common/types.h"
#include "middleware/message_queue.h"
#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>

struct _snd_pcm;

namespace smartcam {

class AudioCapture {
public:
    AudioCapture() = default;
    ~AudioCapture();

    bool init(const std::string& device = "plughw:0,0");
    void start();
    void stop();

    void add_client_queue(std::shared_ptr<MessageQueue<AudioFrame>> queue);
    void remove_client_queue(std::shared_ptr<MessageQueue<AudioFrame>> queue);

    bool is_running() const { return running_; }

private:
    void capture_loop();
    uint8_t pcmu_encode(int16_t sample);

    std::vector<std::shared_ptr<MessageQueue<AudioFrame>>> client_queues_;
    std::mutex queues_mutex_;

    _snd_pcm* pcm_ = nullptr;
    std::string device_;
    std::atomic<bool> running_{false};
    std::thread capture_thread_;
};

} // namespace smartcam

#endif // SMARTCAM_MODULES_AUDIO_CAPTURE_H
```

- [ ] **步骤 2：创建 audio_capture.cpp**

核心实现包含：
- ALSA PCM 设备打开和参数配置（8000Hz, 单声道, 16-bit signed LE）
- capture_loop：循环读取 PCM 数据，G.711 PCMU 编码，分发到客户端队列
- G.711 μ-law 编码查表（标准 ITU-T G.711 μ-law 表，8 位输出）
- add_client_queue / remove_client_queue（与 CameraCapture 模式一致）

ALSA 初始化参数：
- 采样率：8000 Hz
- 通道数：1（单声道）
- 采样格式：SND_PCM_FORMAT_S16_LE
- 每次读取周期：160 帧（20ms @8kHz）
- PCM 缓冲区大小：4 个周期

G.711 PCMU 编码：使用标准 μ-law 查表（13 段折线，8 位编码），将 16-bit signed PCM 转换为 8-bit μ-law。

- [ ] **步骤 3：编译验证**

运行：`cd /home/dministrator/work/smartcam/build && cmake .. && make -j$(nproc)`
预期：编译通过（需确保 CMakeLists.txt 已更新，见任务 7）

- [ ] **步骤 4：Commit**

```bash
git add src/modules/audio/audio_capture.h src/modules/audio/audio_capture.cpp
git commit -m "feat: add AudioCapture module with ALSA and G.711 PCMU encoding"
```

---

### 任务 3：扩展 ClientSession 和 RtspServer 头文件

**文件：**
- 修改：`src/modules/streaming/rtsp_server.h`

- [ ] **步骤 1：在 ClientSession 中新增音频字段**

在现有 `ClientSession` 结构体中，`frame_queue` 之后添加：

```cpp
    // 音频传输参数
    int audio_client_rtp_port = 0;
    int audio_client_rtcp_port = 0;
    int audio_server_rtp_port = 0;
    int audio_server_rtcp_port = 0;
    int audio_rtp_sock = -1;
    int audio_rtp_channel = 2;
    int audio_rtcp_channel = 3;
    std::atomic<bool> audio_rtp_running{false};
    std::thread audio_rtp_thread;
    std::shared_ptr<MessageQueue<AudioFrame>> audio_frame_queue;
    bool audio_setup_done = false;
```

- [ ] **步骤 2：在 RtspServer 类中新增音频方法和成员**

在 `set_camera` 之后添加：

```cpp
    void set_audio(std::shared_ptr<AudioCapture> audio);
```

在 `send_rtp_stream_tcp` 之后添加音频 RTP 发送方法：

```cpp
    // 音频 RTP streaming
    void send_audio_rtp_stream(int rtp_sock, const std::string& client_ip, int client_rtp_port,
        std::atomic<bool>& client_playing, std::atomic<bool>& audio_rtp_running,
        std::shared_ptr<MessageQueue<AudioFrame>> audio_frame_queue,
        uint32_t video_ssrc, uint64_t video_start_ntp, uint32_t video_start_rtp);
    void send_audio_rtp_stream_tcp(int fd, int rtp_channel, int rtcp_channel,
        std::function<void(int, const uint8_t*, size_t)> send_fn,
        std::atomic<bool>& client_playing, std::atomic<bool>& audio_rtp_running,
        std::shared_ptr<MessageQueue<AudioFrame>> audio_frame_queue,
        uint32_t video_ssrc, uint64_t video_start_ntp, uint32_t video_start_rtp);

    // RTCP
    void send_rtcp_sr(int sock, const std::string& client_ip, int client_rtcp_port,
        uint32_t ssrc, uint64_t ntp_timestamp, uint32_t rtp_timestamp,
        uint32_t packet_count, uint32_t octet_count);
    void send_rtcp_sr_tcp(std::function<void(int, const uint8_t*, size_t)> send_fn,
        int rtcp_channel, uint32_t ssrc, uint64_t ntp_timestamp, uint32_t rtp_timestamp,
        uint32_t packet_count, uint32_t octet_count);
```

在 `camera_` 之后添加：

```cpp
    std::shared_ptr<AudioCapture> audio_;
```

- [ ] **步骤 3：编译验证**

运行：`cd /home/dministrator/work/smartcam/build && cmake .. && make -j$(nproc)`
预期：编译失败（方法未实现），但头文件语法正确

- [ ] **步骤 4：Commit**

```bash
git add src/modules/streaming/rtsp_server.h
git commit -m "feat: extend ClientSession and RtspServer with audio fields and methods"
```

---

### 任务 4：实现 SDP 双轨道和音频 SETUP

**文件：**
- 修改：`src/modules/streaming/rtsp_server.cpp`

- [ ] **步骤 1：实现 set_audio 方法**

```cpp
void RtspServer::set_audio(std::shared_ptr<AudioCapture> audio) {
    audio_ = audio;
}
```

- [ ] **步骤 2：修改 handle_describe 输出双轨道 SDP**

将现有 SDP 修改为：

```cpp
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
```

- [ ] **步骤 3：修改 handle_setup 区分视频和音频 SETUP**

在 `handle_setup` 方法开头添加 trackID 解析逻辑：

```cpp
// 判断是视频还是音频 SETUP
bool is_audio_setup = (request.find("trackID=1") != std::string::npos ||
                       request.find("trackid=1") != std::string::npos);
```

然后根据 `is_audio_setup` 分支处理：
- 视频分支：保持现有逻辑不变
- 音频分支：使用 `sess.audio_*` 字段，逻辑与视频 SETUP 完全对称

音频 UDP SETUP 响应示例：
```
Transport: RTP/AVP;unicast;client_port=<audio_rtp>-<audio_rtcp>;server_port=<audio_srv_rtp>-<audio_srv_rtcp>
Session: <session_id>
```

音频 TCP interleaved SETUP 响应示例：
```
Transport: RTP/AVP/TCP;unicast;interleaved=<audio_rtp_channel>-<audio_rtcp_channel>
Session: <session_id>
```

- [ ] **步骤 4：编译验证**

运行：`cd /home/dministrator/work/smartcam/build && cmake .. && make -j$(nproc)`
预期：编译通过

- [ ] **步骤 5：Commit**

```bash
git add src/modules/streaming/rtsp_server.cpp
git commit -m "feat: implement dual-track SDP and audio SETUP in RTSP server"
```

---

### 任务 5：实现音频 RTP 发送

**文件：**
- 修改：`src/modules/streaming/rtsp_server.cpp`

- [ ] **步骤 1：实现 send_audio_rtp_stream（UDP 模式）**

关键参数：
- PT=0（PCMU 静态负载类型）
- 时钟频率 8000Hz
- 每包 160 字节（20ms @8kHz），时间戳增量 160
- 独立 SSRC（随机生成）
- 每 5 秒发送一次 RTCP Sender Report

实现逻辑：
1. 从 `audio_frame_queue` 取 AudioFrame
2. 按 160 字节切分，每片构造 RTP 包发送
3. 维护 seq、timestamp、packet_count、octet_count
4. 每 5 秒调用 send_rtcp_sr

- [ ] **步骤 2：实现 send_audio_rtp_stream_tcp（TCP interleaved 模式）**

与 UDP 版本逻辑相同，但通过 `send_fn` 发送 TCP interleaved 帧。
RTCP SR 通过 `rtcp_channel` 发送。

- [ ] **步骤 3：修改 handle_play 启动音频 RTP 线程**

在现有视频 RTP 线程启动之后，添加音频 RTP 线程启动逻辑：

```cpp
// 启动音频 RTP
if (audio_ && sess.audio_setup_done) {
    sess.audio_frame_queue = std::make_shared<MessageQueue<AudioFrame>>(64);
    audio_->add_client_queue(sess.audio_frame_queue);
    sess.audio_rtp_running = true;

    if (sess.tcp_interleaved) {
        sess.audio_rtp_thread = std::thread([this, &sess]() {
            auto send_fn = [this, &sess](int ch, const uint8_t* d, size_t l) {
                send_tcp_rtp(sess, ch, d, l);
            };
            send_audio_rtp_stream_tcp(sess.fd, sess.audio_rtp_channel,
                sess.audio_rtcp_channel, send_fn,
                sess.client_playing, sess.audio_rtp_running,
                sess.audio_frame_queue, video_ssrc, video_start_ntp, video_start_rtp);
        });
    } else if (sess.audio_rtp_sock >= 0) {
        sess.audio_rtp_thread = std::thread([this, &sess]() {
            send_audio_rtp_stream(sess.audio_rtp_sock, sess.client_ip,
                sess.audio_client_rtp_port,
                sess.client_playing, sess.audio_rtp_running,
                sess.audio_frame_queue, video_ssrc, video_start_ntp, video_start_rtp);
        });
    }
}
```

注意：需要在 PLAY 时记录视频流的 SSRC、起始 NTP 时间和 RTP 时间戳，传递给音频 RTP 线程用于 RTCP SR 同步。

- [ ] **步骤 4：修改 handle_teardown 停止音频**

在现有 teardown 逻辑中添加：

```cpp
sess.audio_rtp_running = false;
if (sess.audio_rtp_thread.joinable()) sess.audio_rtp_thread.join();
if (sess.audio_frame_queue && audio_) {
    audio_->remove_client_queue(sess.audio_frame_queue);
}
if (sess.audio_rtp_sock >= 0) { close(sess.audio_rtp_sock); sess.audio_rtp_sock = -1; }
```

同样在 `handle_client` 的 cleanup 部分添加相同逻辑。

- [ ] **步骤 5：编译验证**

运行：`cd /home/dministrator/work/smartcam/build && cmake .. && make -j$(nproc)`
预期：编译通过

- [ ] **步骤 6：Commit**

```bash
git add src/modules/streaming/rtsp_server.cpp
git commit -m "feat: implement audio RTP streaming with UDP and TCP interleaved"
```

---

### 任务 6：实现 RTCP Sender Report

**文件：**
- 修改：`src/modules/streaming/rtsp_server.cpp`

- [ ] **步骤 1：实现 send_rtcp_sr（UDP 模式）**

RTCP SR 包格式（RFC 3550 Section 6.4.1）：

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|    RC   |   PT=200      |       length                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         SSRC                                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|              NTP timestamp (most significant word)            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|             NTP timestamp (least significant word)            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       RTP timestamp                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   sender's packet count                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    sender's octet count                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

总长度：28 字节。length 字段 = 6（以 32-bit 字为单位，不含头部 4 字节）。

NTP 时间戳获取：使用 `clock_gettime(CLOCK_REALTIME)` 转换为 NTP 格式（秒 << 32 + 纳秒 << 32 / 1000000000）。

- [ ] **步骤 2：实现 send_rtcp_sr_tcp（TCP interleaved 模式）**

与 UDP 版本相同，但通过 `send_fn` 和 `rtcp_channel` 发送。

- [ ] **步骤 3：在视频 RTP 发送线程中添加 RTCP SR 发送**

在 `send_rtp_stream` 和 `send_rtp_stream_tcp` 中，每 5 秒调用一次 `send_rtcp_sr`。

需要维护的计数器：
- `packet_count`：已发送 RTP 包数
- `octet_count`：已发送 RTP 字节数（仅负载，不含头部）

- [ ] **步骤 4：编译验证**

运行：`cd /home/dministrator/work/smartcam/build && cmake .. && make -j$(nproc)`
预期：编译通过

- [ ] **步骤 5：Commit**

```bash
git add src/modules/streaming/rtsp_server.cpp
git commit -m "feat: implement RTCP Sender Report for A/V sync"
```

---

### 任务 7：集成到 MainService 和 CMakeLists

**文件：**
- 修改：`src/app/main_service.h`
- 修改：`src/app/main_service.cpp`
- 修改：`CMakeLists.txt`

- [ ] **步骤 1：修改 main_service.h**

添加 include 和成员：

```cpp
#include "modules/audio/audio_capture.h"

// 在类中添加：
std::shared_ptr<AudioCapture> audio_;
```

- [ ] **步骤 2：修改 main_service.cpp**

在 `setup_modules` 中：

```cpp
audio_ = std::make_shared<AudioCapture>();
rtsp_->set_audio(audio_);
```

在 `run` 中，`camera_->start()` 之后：

```cpp
if (!audio_->init(config_.streaming.audio_device)) {
    SPDLOG_WARN("Failed to initialize audio capture, continuing without audio");
    audio_.reset();
}
if (audio_) audio_->start();
```

在 `shutdown` 中，`camera_->stop()` 之前：

```cpp
if (audio_) audio_->stop();
```

- [ ] **步骤 3：修改 CMakeLists.txt**

在 `SMARTCAM_SOURCES` 中添加：

```cmake
src/modules/audio/audio_capture.cpp
```

在 `target_link_libraries` 中添加：

```cmake
asound
```

- [ ] **步骤 4：编译验证**

运行：`cd /home/dministrator/work/smartcam/build && cmake .. && make -j$(nproc)`
预期：编译通过

- [ ] **步骤 5：Commit**

```bash
git add src/app/main_service.h src/app/main_service.cpp CMakeLists.txt
git commit -m "feat: integrate AudioCapture into MainService and build system"
```

---

### 任务 8：编译部署到香橙派

**文件：** 无代码修改

- [ ] **步骤 1：确保本地编译通过**

运行：`cd /home/dministrator/work/smartcam/build && cmake .. && make -j$(nproc)`
预期：编译通过，生成 `smartcam` 可执行文件

- [ ] **步骤 2：通过 scp 部署到香橙派**

```bash
scp /home/dministrator/work/smartcam/build/smartcam orangepi@100.111.77.78:/tmp/smartcam
```

密码：orangepi

- [ ] **步骤 3：在香橙派上确认 libasound2 已安装**

```bash
ssh orangepi@100.111.77.78 "dpkg -l | grep libasound2 || sudo apt-get install -y libasound2"
```

- [ ] **步骤 4：在香橙派上运行验证**

```bash
ssh orangepi@100.111.77.78 "/tmp/smartcam -c /etc/smartcam/config.yaml"
```

预期：服务启动，日志显示音频采集初始化成功，RTSP 服务器支持音视频双轨道

- [ ] **步骤 5：用 VLC 或 ffplay 验证音视频推流**

```bash
ffplay rtsp://100.111.77.78:8554/live
```

预期：同时播放视频和音频

- [ ] **步骤 6：Commit 最终状态**

```bash
git add -A
git commit -m "feat: complete RTSP audio streaming with RTCP, deployed to Orange Pi"
```
