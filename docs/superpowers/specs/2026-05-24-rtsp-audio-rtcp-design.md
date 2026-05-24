# RTSP 音频推流 + RTCP 协议设计

## 概述

为 SmartCam 项目添加音频采集和推流能力，使 RTSP 服务器支持音视频双轨道推流，并实现 RTCP Sender Report 协议用于音视频同步。

## 方案选择

采用**单 Session 双媒体流模式**：一个 RTSP Session 同时包含视频和音频，SDP 声明两个 `m=` 行，视频和音频分别 SETUP 但共享同一个 Session ID。

## 1. 音频采集模块 (AudioCapture)

### 1.1 新增类型

```cpp
// types.h
struct AudioFrame {
    uint64_t timestamp = 0;        // 微秒
    std::vector<uint8_t> data;     // G.711 PCMU 编码数据
};
```

### 1.2 AudioCapture 类

- **采集**：通过 ALSA API 从麦克风设备（默认 `plughw:0,0`）采集 8000Hz / 单声道 / 16-bit PCM
- **编码**：G.711 PCMU（μ-law）查表转换，无需 FFmpeg 编码器
- **输出**：编码后的音频帧通过 `MessageQueue<AudioFrame>` 分发给 RTSP 客户端
- **接口**：与 CameraCapture 一致，提供 `add_client_queue/remove_client_queue` 方法

### 1.3 配置

`StreamingConfig` 新增字段：
- `audio_device`：ALSA 设备名，默认 `plughw:0,0`

## 2. RTSP 协议扩展

### 2.1 SDP 扩展

`handle_describe` 返回双轨道 SDP：

```
v=0
o=- 0 0 IN IP4 <local_ip>
s=SmartCam
c=IN IP4 <local_ip>
t=0 0
m=video 0 RTP/AVP 96
a=rtpmap:96 H264/90000
a=control:trackID=0
a=fmtp:96 packetization-mode=1;...
m=audio 0 RTP/AVP 0
a=rtpmap:0 PCMU/8000
a=control:trackID=1
```

### 2.2 SETUP 扩展

- 客户端对音频轨道单独发起 SETUP（URL 带 `trackID=1`）
- `ClientSession` 新增音频传输参数：
  - `audio_client_rtp_port` / `audio_client_rtcp_port`
  - `audio_server_rtp_port` / `audio_server_rtcp_port`
  - `audio_rtp_sock`
  - `audio_rtp_channel` / `audio_rtcp_channel`（TCP interleaved）
  - `audio_frame_queue`
  - `audio_rtp_thread`
  - `audio_rtp_running`
- 区分视频 SETUP 和音频 SETUP（根据 URL 中的 trackID）
- 音频支持 TCP interleaved 和 UDP 两种模式

### 2.3 PLAY 扩展

- PLAY 时同时启动音频 RTP 发送线程
- 音频和视频 RTP 线程并行运行

### 2.4 TEARDOWN 扩展

- 同时停止音频 RTP 线程
- 关闭音频 socket
- 移除音频 frame queue

## 3. 音频 RTP 发送

- G.711 PCMU 的 RTP 打包：每个 RTP 包承载 20ms 音频（160 字节 @8kHz）
- RTP 头部 PT=0（PCMU 静态负载类型），时间戳增量 160，时钟频率 8000Hz
- 独立的音频 RTP 发送线程，从 `AudioFrame` 队列取数据后按 160 字节切分打包
- 支持 UDP 和 TCP interleaved 两种模式

## 4. RTCP 协议

### 4.1 Sender Report (SR)

服务器定期发送（约每 5 秒一次），包含：
- NTP 时间戳（绝对时间）
- RTP 时间戳（与 RTP 流中的时间戳对应）
- 发送包计数和字节数

分别为视频流和音频流发送独立的 SR，客户端据此做音视频同步（lip sync）。

### 4.2 传输方式

- UDP 模式：SR 发送到客户端的 RTCP 端口
- TCP interleaved 模式：SR 通过 rtcp_channel 发送

### 4.3 RTCP SR 数据结构

```cpp
struct RtcpSenderReport {
    uint8_t  version = 2;
    uint8_t  pt = 200;          // SR
    uint32_t ssrc;
    uint64_t ntp_timestamp;     // NTP 时间
    uint32_t rtp_timestamp;     // RTP 时间戳
    uint32_t packet_count;      // 已发送 RTP 包数
    uint32_t octet_count;       // 已发送 RTP 字节数
};
```

## 5. 集成

### 5.1 MainService

- 新增 `AudioCapture` 实例，与 `CameraCapture` 并列
- `RtspServer` 新增 `set_audio()` 方法绑定 `AudioCapture`
- 启动/停止顺序：先启音频采集，再启 RTSP

### 5.2 新增文件

- `src/modules/audio/audio_capture.h`
- `src/modules/audio/audio_capture.cpp`

### 5.3 修改文件

- `src/common/types.h` — 新增 `AudioFrame`
- `src/common/config.h` — `StreamingConfig` 新增 `audio_device`
- `src/modules/streaming/rtsp_server.h` — ClientSession 扩展、新增方法
- `src/modules/streaming/rtsp_server.cpp` — SDP、SETUP、PLAY、音频 RTP、RTCP SR
- `src/app/main_service.h` — 新增 `AudioCapture` 成员
- `src/app/main_service.cpp` — 初始化和启动音频模块
- `CMakeLists.txt` — 新增源文件，链接 `asound`

## 6. 部署

- 编译后通过 scp 部署到香橙派（100.111.77.78）
- 确保目标设备安装 `libasound2`
