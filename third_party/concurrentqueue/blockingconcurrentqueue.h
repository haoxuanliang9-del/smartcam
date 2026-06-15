#ifndef BLOCKINGCONCURRENTQUEUE_H
#define BLOCKINGCONCURRENTQUEUE_H

#include <mutex>
#include <condition_variable>
#include <queue>
#include <chrono>
#include <cstddef>

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

    bool try_enqueue(T&& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(item));
        cv_.notify_one();
        return true;
    }

    template<typename U>
    bool wait_dequeue_timed(U& item, std::int64_t timeout_usecs) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, std::chrono::microseconds(timeout_usecs),
                         [this] { return !queue_.empty(); })) {
            item = std::move(queue_.front());
            queue_.pop();
            return true;
        }
        return false;
    }

    template<typename U, typename Rep, typename Period>
    bool wait_dequeue_timed(U& item, std::chrono::duration<Rep, Period> const& timeout) {
        return wait_dequeue_timed(item, std::chrono::duration_cast<std::chrono::microseconds>(timeout).count());
    }

    template<typename U>
    void wait_dequeue(U& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty(); });
        item = std::move(queue_.front());
        queue_.pop();
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
