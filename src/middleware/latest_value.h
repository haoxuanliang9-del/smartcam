#ifndef SMARTCAM_MIDDLEWARE_LATEST_VALUE_H
#define SMARTCAM_MIDDLEWARE_LATEST_VALUE_H

#include <memory>
#include <mutex>
#include <condition_variable>
#include <chrono>

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

} // namespace smartcam

#endif // SMARTCAM_MIDDLEWARE_LATEST_VALUE_H
