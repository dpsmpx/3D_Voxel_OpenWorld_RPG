#pragma once
#include "types.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace jobs {

using JobFn = void(*)(void*);

// Счётчик для отслеживания завершения группы задач.
// Можно инкрементировать из любого потока, декрементировать по завершении.
class Counter {
public:
    void add(i32 n = 1) { count_.fetch_add(n, std::memory_order_relaxed); }
    void done() {
        if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard lk(mtx_);
            cv_.notify_all();
        }
    }
    void wait() {
        std::unique_lock lk(mtx_);
        cv_.wait(lk, [this]{ return count_.load(std::memory_order_acquire) == 0; });
    }
    bool isZero() const { return count_.load(std::memory_order_acquire) == 0; }

private:
    std::atomic<i32>        count_{0};
    std::mutex              mtx_;
    std::condition_variable cv_;
};

class JobSystem {
public:
    JobSystem() = default;
    ~JobSystem() { stop(); }

    void start(u32 workerCount = 0);
    void stop();

    // Отправить одиночную задачу. Из главного потока — в глобальную очередь.
    // Из воркера — в собственную (для locality).
    void submit(JobFn fn, void* data = nullptr);

    // Отправить с отслеживанием: counter->add() вызывается автоматически.
    void submit(Counter* c, JobFn fn, void* data = nullptr);

    // Параллельный цикл [0, count), разбитый на куски не меньше minChunk.
    // fn(begin, end) вызывается на воркерах. Блокирует до завершения.
    void parallelFor(u32 count, u32 minChunk,
                     const std::function<void(u32,u32)>& fn);

    u32 workerCount() const { return (u32)workers_.size(); }
    u32 currentWorkerId() const;   // UINT32_MAX для внешних потоков

public:
    struct Job {
        JobFn fn;
        void* data;
        Counter* counter = nullptr;  // nullptr = без отслеживания
    };

    // Per-worker state
    struct Worker {
        std::thread             thread;
        std::deque<Job>         local;
        std::mutex              mtx;
    };

    bool popJob(Job& out, u32 workerId);
    bool trySteal(Job& out, u32 myWorkerId);
    void workerLoop(u32 workerId);

    std::vector<std::unique_ptr<Worker>> workers_;
    std::mutex              globalMtx_;
    std::deque<Job>         globalQueue_;
    std::condition_variable wakeup_;
    std::atomic<bool>       running_{false};
    std::atomic<u32>        activeJobs_{0};
    static thread_local u32 tlsWorkerId_;
    friend struct ParallelRange;
};

extern JobSystem gJobs;

} // namespace jobs
