// Placeholder - download from https://github.com/bshoshany/thread-pool
// This header-only library will be installed during build setup on the target device
#ifndef BS_THREAD_POOL_HPP
#define BS_THREAD_POOL_HPP

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>

namespace BS {

class thread_pool {
public:
    explicit thread_pool(uint32_t num_threads = std::thread::hardware_concurrency())
        : stop_(false) {
        for (uint32_t i = 0; i < num_threads; i++) {
            workers_.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                }
            });
        }
    }

    ~thread_pool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) w.join();
    }

    template<typename F>
    auto submit(F&& f) -> std::future<decltype(f())> {
        using ReturnType = decltype(f());
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(std::forward<F>(f));
        auto result = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) throw std::runtime_error("submit on stopped pool");
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return result;
    }

    void wait_for_tasks() {}

    uint32_t get_thread_count() const { return static_cast<uint32_t>(workers_.size()); }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_;
};

} // namespace BS

#endif // BS_THREAD_POOL_HPP
