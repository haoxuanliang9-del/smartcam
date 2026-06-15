#ifndef SMARTCAM_MIDDLEWARE_MESSAGE_QUEUE_H
#define SMARTCAM_MIDDLEWARE_MESSAGE_QUEUE_H

#include <blockingconcurrentqueue.h>
#include <memory>
#include <chrono>
#include <atomic>

namespace smartcam {

template<typename T>
class MessageQueue {
public:
    explicit MessageQueue(size_t capacity = 1024)
        : queue_(capacity), capacity_(capacity) {}

    // 推入帧，队列满时直接返回 false（不丢弃旧帧）
    bool push(const T& item) {
        return queue_.try_enqueue(item);
    }

    bool push(T&& item) {
        return queue_.try_enqueue(std::move(item));
    }

    bool pop(T& item, int timeout_ms = 1000) {
        return queue_.wait_dequeue_timed(item, std::chrono::milliseconds(timeout_ms));
    }

    // 移动语义 pop：从队列取出后清空原 slot，避免 shared_ptr 引用计数残留
    bool pop_move(T& item, int timeout_ms = 1000) {
        bool ok = queue_.wait_dequeue_timed(item, std::chrono::milliseconds(timeout_ms));
        return ok;
    }

    size_t size_approx() const {
        return queue_.size_approx();
    }

    size_t capacity() const {
        return capacity_;
    }

    // 清空队列中所有帧
    void clear() {
        T item;
        while (queue_.wait_dequeue_timed(item, std::chrono::milliseconds(0))) {}
    }

    // 同步等待状态：拥塞后等待 IDR 帧重新同步
    bool is_waiting_for_sync() const {
        return waiting_for_sync_.load(std::memory_order_relaxed);
    }

    void set_waiting_for_sync(bool value) {
        waiting_for_sync_.store(value, std::memory_order_relaxed);
    }

private:
    moodycamel::BlockingConcurrentQueue<T> queue_;
    size_t capacity_;
    std::atomic<bool> waiting_for_sync_{false};
};

} // namespace smartcam

#endif // SMARTCAM_MIDDLEWARE_MESSAGE_QUEUE_H
