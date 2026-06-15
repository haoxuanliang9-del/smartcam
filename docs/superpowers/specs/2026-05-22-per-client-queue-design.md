# 多客户端独立帧队列设计

## 背景

当前架构使用单一共享帧队列，编码器生产帧（25fps），RTSP 发送端消费帧（固定 15fps）。存在以下问题：

1. **生产 > 消费**：队列逐渐堆积，最终满则丢帧
2. **延迟累积**：网络波动后积压的旧帧持续播放，观众永远看到滞后画面
3. **多客户端干扰**：共享队列导致快客户端被慢客户端拖累

## 目标

1. 取消发送速率限制，让发送速度匹配网络实际能力
2. RTP 时间戳按采集时间计算，保证时序正确
3. 每个客户端独立队列，互不干扰
4. 队列容量有限（64 帧），延迟有上界

## 架构变更

### 当前架构

```
[CameraCapture] → [共享队列 1024帧] → [Client Thread 1]
                                   → [Client Thread 2]
                                   → [Client Thread 3]
```

### 新架构

```
[CameraCapture] ──┬──→ [Queue 64帧] → [Client Thread 1]
                  ├──→ [Queue 64帧] → [Client Thread 2]
                  └──→ [Queue 64帧] → [Client Thread 3]
```

## 详细设计

### 1. CameraCapture 变更

**新增接口**：

```cpp
// camera_capture.h
class CameraCapture {
public:
    void add_client_queue(std::shared_ptr<MessageQueue<Frame>> queue);
    void remove_client_queue(std::shared_ptr<MessageQueue<Frame>> queue);

private:
    std::vector<std::shared_ptr<MessageQueue<Frame>>> client_queues_;
    std::mutex queues_mutex_;
};
```

**帧分发逻辑**：

```cpp
// camera_capture.cpp - capture_loop()
{
    std::lock_guard<std::mutex> lock(queues_mutex_);
    for (auto& queue : client_queues_) {
        queue->push(output);  // 队列满则静默丢弃
    }
}
```

**移除原有 output_queue_ 成员**，改为使用 client_queues_ 列表。

### 2. RtspServer 变更

**ClientSession 新增队列成员**：

```cpp
// rtsp_server.h
struct ClientSession {
    // ... 现有成员 ...
    std::shared_ptr<MessageQueue<Frame>> frame_queue;
};
```

**RtspServer 新增队列管理接口**：

```cpp
// rtsp_server.h
class RtspServer {
public:
    void set_camera(std::shared_ptr<CameraCapture> camera);

private:
    std::shared_ptr<CameraCapture> camera_;
};
```

**客户端连接时创建队列**：

```cpp
// handle_play()
sess.frame_queue = std::make_shared<MessageQueue<Frame>>(64);
camera_->add_client_queue(sess.frame_queue);
```

**客户端断开时销毁队列**：

```cpp
// handle_teardown() 或连接断开
if (sess.frame_queue && camera_) {
    camera_->remove_client_queue(sess.frame_queue);
}
```

### 3. RTP 发送逻辑变更

**取消速率限制**：

删除 `send_rtp_stream_tcp()` 和 `send_rtp_stream()` 中的以下代码：

```cpp
// 删除
next_frame_time += std::chrono::microseconds(66667);
auto now = std::chrono::steady_clock::now();
if (next_frame_time > now) {
    std::this_thread::sleep_until(next_frame_time);
} else {
    next_frame_time = now;
}
```

**RTP 时间戳按采集时间计算**：

```cpp
// 当前
timestamp += 6000;

// 改为
timestamp = static_cast<uint32_t>(frame.timestamp * 90 / 1000000);
```

### 4. MainService 变更

**移除共享 frame_queue_**：

```cpp
// main_service.h - 删除
// std::shared_ptr<MessageQueue<Frame>> frame_queue_ = ...;
```

**注入 CameraCapture 到 RtspServer**：

```cpp
// main_service.cpp - setup_modules()
camera_ = std::make_unique<CameraCapture>();  // 不再传入队列
rtsp_ = std::make_unique<RtspServer>();
rtsp_->set_camera(camera_);  // 注入依赖
```

## 文件改动清单

| 文件 | 改动 |
|------|------|
| `camera_capture.h` | 添加 `add_client_queue` / `remove_client_queue`，移除 `output_queue_` |
| `camera_capture.cpp` | 修改构造函数，修改 `capture_loop` 分发逻辑 |
| `rtsp_server.h` | `ClientSession` 添加 `frame_queue`，添加 `set_camera` 接口 |
| `rtsp_server.cpp` | 取消速率限制，RTP 时间戳按采集时间计算，客户端队列生命周期管理 |
| `main_service.h` | 移除 `frame_queue_` 成员 |
| `main_service.cpp` | 修改 `setup_modules()` |

## 行为变更

| 场景 | 当前行为 | 新行为 |
|------|---------|--------|
| 网络正常 | 队列空，15fps 发送 | 队列空，按网络速度发送（~25fps） |
| 网络卡顿 | 队列堆积，恢复后播放旧帧 | 队列满则丢帧，恢复后播放最新帧 |
| 多客户端 | 共享队列，互相干扰 | 独立队列，互不影响 |
| 延迟上限 | 1024 帧 ≈ 40 秒 | 64 帧 ≈ 2.5 秒 |

## 风险与缓解

| 风险 | 缓解措施 |
|------|---------|
| UDP 突发丢包 | 接受可能的丢包，依赖客户端重传或 FEC |
| 队列满丢帧 | 静默丢弃，可后续添加日志监控 |
| 编码器分发开销 | 遍历 3-5 个队列的开销可忽略 |

## 测试计划

1. **单客户端测试**：验证无限速下发流畅，时间戳正确
2. **网络波动测试**：模拟卡顿，验证恢复后播放最新帧
3. **多客户端测试**：验证独立队列互不干扰
4. **压力测试**：3 客户端长时间运行，监控内存和延迟
