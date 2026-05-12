#ifndef SMARTCAM_MIDDLEWARE_TIMER_MANAGER_H
#define SMARTCAM_MIDDLEWARE_TIMER_MANAGER_H

#include <asio.hpp>
#include <functional>
#include <map>
#include <atomic>

namespace smartcam {

class TimerManager {
public:
    explicit TimerManager(asio::io_context& io_ctx)
        : io_ctx_(io_ctx), next_id_(1) {}

    using TimerId = uint64_t;
    using TimerCallback = std::function<void()>;

    TimerId set_interval(uint32_t interval_ms, TimerCallback cb) {
        TimerId id = next_id_++;
        auto timer = std::make_shared<asio::steady_timer>(
            io_ctx_, std::chrono::milliseconds(interval_ms));

        timers_[id] = timer;

        auto weak_timer = std::weak_ptr<asio::steady_timer>(timer);
        timer->async_wait([this, id, interval_ms, cb = std::move(cb), weak_timer](
            const std::error_code& ec) {
            if (ec) return;
            cb();
            auto t = weak_timer.lock();
            if (t && timers_.count(id)) {
                t->expires_after(std::chrono::milliseconds(interval_ms));
                t->async_wait([this, id, interval_ms, cb, weak_timer](
                    const std::error_code& ec2) {
                    if (ec2) return;
                    cb();
                    auto t2 = weak_timer.lock();
                    if (t2 && timers_.count(id)) {
                        t2->expires_after(std::chrono::milliseconds(interval_ms));
                        t2->async_wait([](const std::error_code&) {});
                    }
                });
            }
        });

        return id;
    }

    TimerId set_timeout(uint32_t delay_ms, TimerCallback cb) {
        TimerId id = next_id_++;
        auto timer = std::make_shared<asio::steady_timer>(
            io_ctx_, std::chrono::milliseconds(delay_ms));

        timers_[id] = timer;

        timer->async_wait([this, id, cb = std::move(cb)](const std::error_code& ec) {
            if (!ec) cb();
            timers_.erase(id);
        });

        return id;
    }

    void cancel(TimerId id) {
        auto it = timers_.find(id);
        if (it != timers_.end()) {
            it->second->cancel();
            timers_.erase(it);
        }
    }

    void cancel_all() {
        for (auto& [id, timer] : timers_) {
            timer->cancel();
        }
        timers_.clear();
    }

private:
    asio::io_context& io_ctx_;
    std::map<TimerId, std::shared_ptr<asio::steady_timer>> timers_;
    std::atomic<uint64_t> next_id_;
};

} // namespace smartcam

#endif // SMARTCAM_MIDDLEWARE_TIMER_MANAGER_H
