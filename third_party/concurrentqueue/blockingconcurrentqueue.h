// Placeholder - download from https://github.com/cameron314/concurrentqueue
// This header-only library will be installed during build setup on the target device
#ifndef BLOCKINGCONCURRENTQUEUE_H
#define BLOCKINGCONCURRENTQUEUE_H

#include <mutex>
#include <condition_variable>
#include <queue>
#include <chrono>

namespace moodycamel {

template<typename T>
class BlockingConcurrentQueue {
public:
    explicit BlockingConcurrentQueue(size_t = 1024) {}

    bool try_enqueue(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(item);
        cv_.notify_one();
        return true;
    }

    bool wait_dequeue_timed(T& item, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
            item = std::move(queue_.front());
            queue_.pop();
            return true;
        }
        return false;
    }

    size_t size_approx() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
};

} // namespace moodycamel

#endif // BLOCKINGCONCURRENTQUEUE_H
