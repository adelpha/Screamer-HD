#pragma once
// A fixed set of worker threads that run numbered jobs: run(n, fn) calls fn(0..n-1), spread
// over the workers and the calling thread, and returns when all are done.

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace scr {

class WorkerPool {
public:
    // threads: how many run jobs, the caller included (<= 0: one per hardware thread).
    explicit WorkerPool(int threads = 0);
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    int threads() const { return int(workers_.size()) + 1; }
    void run(int jobs, const std::function<void(int)>& fn);

private:
    void work();
    void take_jobs(std::unique_lock<std::mutex>& lock);

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable start_, done_;
    const std::function<void(int)>* fn_ = nullptr;
    int jobs_ = 0, next_ = 0, finished_ = 0;
    unsigned generation_ = 0;
    bool quit_ = false;
};

} // namespace scr
