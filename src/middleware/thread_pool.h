#ifndef SMARTCAM_MIDDLEWARE_THREAD_POOL_H
#define SMARTCAM_MIDDLEWARE_THREAD_POOL_H

#include <BS_thread_pool.hpp>
#include <functional>
#include <future>

namespace smartcam {

class ThreadPool {
public:
    explicit ThreadPool(uint32_t num_threads = 0)
        : pool_(num_threads > 0 ? num_threads : std::thread::hardware_concurrency()) {}

    auto submit(std::function<void()> task) -> std::future<void> {
        return pool_.submit(task);
    }

    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
        return pool_.submit(std::forward<F>(f), std::forward<Args>(args)...);
    }

    uint32_t thread_count() const { return pool_.get_thread_count(); }

    void wait_for_tasks() { pool_.wait_for_tasks(); }

private:
    BS::thread_pool pool_;
};

} // namespace smartcam

#endif // SMARTCAM_MIDDLEWARE_THREAD_POOL_H
