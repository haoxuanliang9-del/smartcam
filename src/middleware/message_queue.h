#ifndef SMARTCAM_MIDDLEWARE_MESSAGE_QUEUE_H
#define SMARTCAM_MIDDLEWARE_MESSAGE_QUEUE_H

#include <blockingconcurrentqueue.h>
#include <memory>
#include <chrono>

namespace smartcam {

template<typename T>
class MessageQueue {
public:
    explicit MessageQueue(size_t capacity = 1024)
        : queue_(capacity) {}

    bool push(const T& item) {
        return queue_.try_enqueue(item);
    }

    bool push(T&& item) {
        return queue_.try_enqueue(std::move(item));
    }

    bool pop(T& item, int timeout_ms = 1000) {
        return queue_.wait_dequeue_timed(item, std::chrono::milliseconds(timeout_ms));
    }

    size_t size_approx() const {
        return queue_.size_approx();
    }

private:
    moodycamel::BlockingConcurrentQueue<T> queue_;
};

} // namespace smartcam

#endif // SMARTCAM_MIDDLEWARE_MESSAGE_QUEUE_H
