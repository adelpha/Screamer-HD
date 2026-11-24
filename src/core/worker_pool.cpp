#include "core/worker_pool.h"

namespace scr {

WorkerPool::WorkerPool(int threads) {
    if (threads <= 0) threads = int(std::thread::hardware_concurrency());
    if (threads < 1) threads = 1;
    for (int i = 1; i < threads; ++i) workers_.emplace_back([this] { work(); });
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    start_.notify_all();
    for (std::thread& t : workers_) t.join();
}

void WorkerPool::take_jobs(std::unique_lock<std::mutex>& lock) {
    while (next_ < jobs_) {
        const int job = next_++;
        const auto* fn = fn_;
        lock.unlock();
        (*fn)(job);
        lock.lock();
        if (++finished_ == jobs_) done_.notify_all();
    }
}

void WorkerPool::work() {
    unsigned seen = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        start_.wait(lock, [&] { return quit_ || generation_ != seen; });
        if (quit_) return;
        seen = generation_;
        take_jobs(lock);
    }
}

void WorkerPool::run(int jobs, const std::function<void(int)>& fn) {
    if (jobs <= 0) return;
    if (workers_.empty() || jobs == 1) {
        for (int i = 0; i < jobs; ++i) fn(i);
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    fn_ = &fn;
    jobs_ = jobs;
    next_ = finished_ = 0;
    ++generation_;
    start_.notify_all();
    take_jobs(lock);
    done_.wait(lock, [&] { return finished_ == jobs_; });
    fn_ = nullptr;
}

} // namespace scr
