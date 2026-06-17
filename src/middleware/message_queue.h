#ifndef SMARTCAM_MIDDLEWARE_MESSAGE_QUEUE_H
#define SMARTCAM_MIDDLEWARE_MESSAGE_QUEUE_H

#include <blockingconcurrentqueue.h>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>

namespace smartcam {

template<typename T>
class LatestValue {
public:
    void put(std::shared_ptr<T> item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            value_ = std::move(item);
            seq_++;
        }
        cv_.notify_one();
    }

    std::shared_ptr<T> get(uint64_t& last_seq, int timeout_ms = 100) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                     [this, &last_seq] { return seq_ != last_seq; });
        if (seq_ == last_seq) return nullptr;
        last_seq = seq_;
        return value_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::shared_ptr<T> value_;
    uint64_t seq_ = 0;
};

template<typename T>
class MessageQueue {
public:
    explicit MessageQueue(size_t capacity = 1024)
        : queue_(capacity), capacity_(capacity) {}

    bool push(const T& item) {
        return queue_.try_enqueue(item);
    }

    bool push(T&& item) {
        return queue_.try_enqueue(std::move(item));
    }

    bool pop(T& item, int timeout_ms = 1000) {
        return queue_.wait_dequeue_timed(item, std::chrono::milliseconds(timeout_ms));
    }

    bool pop_move(T& item, int timeout_ms = 1000) {
        return queue_.wait_dequeue_timed(item, std::chrono::milliseconds(timeout_ms));
    }

    size_t size_approx() const {
        return queue_.size_approx();
    }

    size_t capacity() const {
        return capacity_;
    }

    void clear() {
        T item;
        while (queue_.wait_dequeue_timed(item, std::chrono::milliseconds(0))) {}
    }

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
