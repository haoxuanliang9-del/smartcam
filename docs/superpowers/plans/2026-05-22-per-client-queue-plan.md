# 多客户端独立帧队列实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 实现每客户端独立帧队列，取消发送速率限制，RTP 时间戳按采集时间计算

**架构：** 编码器持有客户端队列列表，帧分发到所有队列；每个客户端线程从独立队列取帧发送；取消发送限速，RTP 时间戳从帧采集时间计算

**技术栈：** C++17, moodycamel::BlockingConcurrentQueue, FFmpeg, RTSP/RTP

---

## 文件结构

| 文件 | 职责 | 改动类型 |
|------|------|---------|
| `camera_capture.h` | 编码器接口，客户端队列管理 | 修改 |
| `camera_capture.cpp` | 帧分发逻辑 | 修改 |
| `rtsp_server.h` | RTSP 服务器，ClientSession 持有队列 | 修改 |
| `rtsp_server.cpp` | RTP 发送逻辑，队列生命周期 | 修改 |
| `main_service.h` | 主服务，移除共享队列 | 修改 |
| `main_service.cpp` | 模块初始化 | 修改 |

---

### 任务 1：修改 CameraCapture 头文件

**文件：**
- 修改：`/home/dministrator/work/smartcam/src/modules/camera/camera_capture.h`

- [ ] **步骤 1：添加客户端队列管理接口**

在 `CameraCapture` 类中添加以下成员：

```cpp
// 在 public 部分添加
void add_client_queue(std::shared_ptr<MessageQueue<Frame>> queue);
void remove_client_queue(std::shared_ptr<MessageQueue<Frame>> queue);

// 在 private 部分添加
std::vector<std::shared_ptr<MessageQueue<Frame>>> client_queues_;
std::mutex queues_mutex_;
```

- [ ] **步骤 2：移除 output_queue_ 成员**

删除以下代码：

```cpp
// 删除这行
std::shared_ptr<MessageQueue<Frame>> output_queue_;
```

- [ ] **步骤 3：修改构造函数声明**

将构造函数从：

```cpp
explicit CameraCapture(std::shared_ptr<MessageQueue<Frame>> output_queue);
```

改为：

```cpp
CameraCapture() = default;
```

---

### 任务 2：修改 CameraCapture 实现文件

**文件：**
- 修改：`/home/dministrator/work/smartcam/src/modules/camera/camera_capture.cpp`

- [ ] **步骤 1：修改构造函数**

删除构造函数中的 `output_queue_` 初始化：

```cpp
// 删除整个构造函数实现，改为默认构造
CameraCapture::CameraCapture() = default;
```

- [ ] **步骤 2：实现 add_client_queue**

在文件末尾添加：

```cpp
void CameraCapture::add_client_queue(std::shared_ptr<MessageQueue<Frame>> queue) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    client_queues_.push_back(queue);
    SPDLOG_INFO("Client queue added, total queues: {}", client_queues_.size());
}
```

- [ ] **步骤 3：实现 remove_client_queue**

在文件末尾添加：

```cpp
void CameraCapture::remove_client_queue(std::shared_ptr<MessageQueue<Frame>> queue) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = std::remove(client_queues_.begin(), client_queues_.end(), queue);
    if (it != client_queues_.end()) {
        client_queues_.erase(it, client_queues_.end());
        SPDLOG_INFO("Client queue removed, remaining queues: {}", client_queues_.size());
    }
}
```

- [ ] **步骤 4：修改 capture_loop 中的帧分发逻辑**

找到 `output_queue_->push(output);` 所在位置（约第 319 行），替换为：

```cpp
{
    std::lock_guard<std::mutex> lock(queues_mutex_);
    for (auto& queue : client_queues_) {
        if (!queue->push(output)) {
            SPDLOG_DEBUG("Frame dropped for client queue, queue full");
        }
    }
}
```

---

### 任务 3：修改 RtspServer 头文件

**文件：**
- 修改：`/home/dministrator/work/smartcam/src/modules/streaming/rtsp_server.h`

- [ ] **步骤 1：添加 CameraCapture 前向声明和头文件引用**

在文件开头添加：

```cpp
namespace smartcam {
class CameraCapture;
}
```

- [ ] **步骤 2：在 ClientSession 结构体中添加 frame_queue 成员**

在 `ClientSession` 结构体中添加：

```cpp
std::shared_ptr<MessageQueue<Frame>> frame_queue;
```

- [ ] **步骤 3：修改 RtspServer 构造函数**

将构造函数从：

```cpp
explicit RtspServer(std::shared_ptr<MessageQueue<Frame>> frame_queue);
```

改为：

```cpp
RtspServer() = default;
```

- [ ] **步骤 4：添加 set_camera 接口**

在 `RtspServer` 类的 public 部分添加：

```cpp
void set_camera(std::shared_ptr<CameraCapture> camera);
```

- [ ] **步骤 5：添加 camera_ 成员和移除 frame_queue_**

在 private 部分添加：

```cpp
std::shared_ptr<CameraCapture> camera_;
```

删除：

```cpp
std::shared_ptr<MessageQueue<Frame>> frame_queue_;
```

- [ ] **步骤 6：更新 send_rtp_stream_tcp 函数声明**

将：

```cpp
void send_rtp_stream_tcp(int fd, int channel,
    std::function<void(int, const uint8_t*, size_t)> send_fn,
    std::atomic<bool>& client_playing, std::atomic<bool>& rtp_running);
```

改为：

```cpp
void send_rtp_stream_tcp(int fd, int channel,
    std::function<void(int, const uint8_t*, size_t)> send_fn,
    std::atomic<bool>& client_playing, std::atomic<bool>& rtp_running,
    std::shared_ptr<MessageQueue<Frame>> frame_queue);
```

- [ ] **步骤 7：更新 send_rtp_stream 函数声明**

将：

```cpp
void send_rtp_stream(int rtp_sock, const std::string& client_ip, int client_rtp_port,
    std::atomic<bool>& client_playing, std::atomic<bool>& rtp_running);
```

改为：

```cpp
void send_rtp_stream(int rtp_sock, const std::string& client_ip, int client_rtp_port,
    std::atomic<bool>& client_playing, std::atomic<bool>& rtp_running,
    std::shared_ptr<MessageQueue<Frame>> frame_queue);
```

---

### 任务 4：修改 RtspServer 实现文件 - 构造和队列管理

**文件：**
- 修改：`/home/dministrator/work/smartcam/src/modules/streaming/rtsp_server.cpp`

- [ ] **步骤 1：添加 CameraCapture 头文件引用**

在文件开头添加：

```cpp
#include "modules/camera/camera_capture.h"
```

- [ ] **步骤 2：修改构造函数**

删除原有的构造函数实现，改为：

```cpp
RtspServer::RtspServer() = default;
```

- [ ] **步骤 3：实现 set_camera 方法**

添加：

```cpp
void RtspServer::set_camera(std::shared_ptr<CameraCapture> camera) {
    camera_ = camera;
}
```

---

### 任务 5：修改 RtspServer 实现文件 - RTP 发送逻辑

**文件：**
- 修改：`/home/dministrator/work/smartcam/src/modules/streaming/rtsp_server.cpp`

- [ ] **步骤 1：修改 send_rtp_stream_tcp 中的帧队列引用**

找到 `frame_queue_->pop(frame, 100);` 所在位置（约第 587 行），改为：

```cpp
bool got = sess_ref.frame_queue->pop(frame, 100);
```

需要通过参数传递 `ClientSession` 引用或 `frame_queue`。修改函数签名：

```cpp
void RtspServer::send_rtp_stream_tcp(int fd, int channel,
    std::function<void(int, const uint8_t*, size_t)> send_fn,
    std::atomic<bool>& client_playing, std::atomic<bool>& rtp_running,
    std::shared_ptr<MessageQueue<Frame>> frame_queue);
```

- [ ] **步骤 2：修改 send_rtp_stream 中的帧队列引用**

同样修改 `send_rtp_stream` 函数签名和内部调用：

```cpp
void RtspServer::send_rtp_stream(int rtp_sock, const std::string& client_ip, int client_rtp_port,
    std::atomic<bool>& client_playing, std::atomic<bool>& rtp_running,
    std::shared_ptr<MessageQueue<Frame>> frame_queue);
```

- [ ] **步骤 3：取消 TCP 模式的发送速率限制**

在 `send_rtp_stream_tcp` 中删除以下代码（约第 600-609 行）：

```cpp
// 删除这些行
next_frame_time += std::chrono::microseconds(66667);
auto now = std::chrono::steady_clock::now();
if (next_frame_time > now) {
    std::this_thread::sleep_until(next_frame_time);
} else {
    next_frame_time = now;
}
```

同时删除 `next_frame_time` 变量声明。

- [ ] **步骤 4：取消 UDP 模式的发送速率限制**

在 `send_rtp_stream` 中删除同样的速率限制代码（约第 700-709 行）。

- [ ] **步骤 5：修改 TCP 模式的 RTP 时间戳计算**

在 `send_rtp_stream_tcp` 中，找到 `timestamp += 6000;`（约第 594 行），改为：

```cpp
timestamp = static_cast<uint32_t>(frame.timestamp * 90 / 1000000);
```

- [ ] **步骤 6：修改 UDP 模式的 RTP 时间戳计算**

在 `send_rtp_stream` 中做同样的修改。

---

### 任务 6：修改 RtspServer 实现文件 - 客户端队列生命周期

**文件：**
- 修改：`/home/dministrator/work/smartcam/src/modules/streaming/rtsp_server.cpp`

- [ ] **步骤 1：在 handle_play 中创建客户端队列**

找到 `handle_play` 函数，在启动 RTP 线程之前添加：

```cpp
sess.frame_queue = std::make_shared<MessageQueue<Frame>>(64);
if (camera_) {
    camera_->add_client_queue(sess.frame_queue);
}
```

- [ ] **步骤 2：修改 RTP 线程启动调用**

修改 `send_rtp_stream_tcp` 调用，传入 `sess.frame_queue`：

```cpp
rtp_thread = std::thread(&RtspServer::send_rtp_stream_tcp, this,
    fd, sess.rtp_channel,
    [this, fd](int ch, const uint8_t* data, size_t len) {
        send_tcp_rtp_by_fd(fd, ch, data, len);
    },
    std::ref(sess.client_playing), std::ref(sess.rtp_running),
    sess.frame_queue);
```

- [ ] **步骤 3：在 handle_teardown 中销毁客户端队列**

找到 `handle_teardown` 函数，添加：

```cpp
if (sess.frame_queue && camera_) {
    camera_->remove_client_queue(sess.frame_queue);
}
```

- [ ] **步骤 4：在客户端连接断开时销毁队列**

在 `handle_client` 函数结束前（线程退出时），添加：

```cpp
if (sess.frame_queue && camera_) {
    camera_->remove_client_queue(sess.frame_queue);
}
```

---

### 任务 7：修改 MainService 头文件

**文件：**
- 修改：`/home/dministrator/work/smartcam/src/app/main_service.h`

- [ ] **步骤 1：移除 frame_queue_ 成员**

删除以下代码：

```cpp
std::shared_ptr<MessageQueue<Frame>> frame_queue_ = std::make_shared<MessageQueue<Frame>>(64);
```

- [ ] **步骤 2：修改 camera_ 成员类型**

将 `camera_` 从 `unique_ptr` 改为 `shared_ptr`：

```cpp
std::shared_ptr<CameraCapture> camera_;
```

---

### 任务 8：修改 MainService 实现文件

**文件：**
- 修改：`/home/dministrator/work/smartcam/src/app/main_service.cpp`

- [ ] **步骤 1：修改 setup_modules 中的模块初始化**

将：

```cpp
camera_ = std::make_unique<CameraCapture>(frame_queue_);
rtsp_ = std::make_unique<RtspServer>(frame_queue_);
```

改为：

```cpp
camera_ = std::make_shared<CameraCapture>();
rtsp_ = std::make_unique<RtspServer>();
rtsp_->set_camera(camera_);
```

---

### 任务 9：编译验证

**文件：**
- 无文件改动

- [ ] **步骤 1：编译项目**

```bash
cd /home/dministrator/work/smartcam && mkdir -p build && cd build && cmake .. && make -j$(nproc)
```

预期：编译成功，无错误

- [ ] **步骤 2：检查编译警告**

如果有警告，分析是否需要修复。

---

### 任务 10：功能测试

**文件：**
- 无文件改动

- [ ] **步骤 1：启动服务**

```bash
cd /home/dministrator/work/smartcam/build && sudo ./smartcam
```

- [ ] **步骤 2：使用 VLC 连接测试**

```
vlc rtsp://<设备IP>:8554/stream
```

预期：画面流畅，无卡顿

- [ ] **步骤 3：测试多客户端**

同时打开多个 VLC 窗口连接同一地址，验证独立队列互不干扰。

---

## Commit 计划

每个任务完成后 commit：

1. 任务 1-2 完成后：`git commit -m "refactor(camera): 改为客户端队列列表分发模式"`
2. 任务 3-6 完成后：`git commit -m "refactor(rtsp): 每客户端独立队列，取消发送限速，RTP时间戳按采集时间"`
3. 任务 7-8 完成后：`git commit -m "refactor(main): 移除共享帧队列，注入CameraCapture到RtspServer"`
4. 任务 9-10 完成后：`git commit -m "test: 验证多客户端独立队列功能"`
