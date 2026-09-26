/**
 * @file job_system.cpp
 * @brief Ядро движка: базовые типы, математика, планировщик задач, аллокаторы.
 */
#include "job_system.h"
#include "log.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace jobs {

thread_local u32 JobSystem::tlsWorkerId_ = 0xFFFFFFFFu;
JobSystem gJobs;

void JobSystem::start(u32 workerCount) {
    if (running_.exchange(true)) return;
    if (workerCount == 0) {
        u32 hw = std::thread::hardware_concurrency();
        workerCount = hw > 1 ? hw - 1 : 1;
    }
    LOGI("JobSystem: старт с %u воркерами", workerCount);
    workers_.reserve(workerCount);
    for (u32 i = 0; i < workerCount; ++i) {
        auto w = std::make_unique<Worker>();
        workers_.push_back(std::move(w));
    }
    for (u32 i = 0; i < workerCount; ++i) {
        workers_[i]->thread = std::thread([this, i]{ workerLoop(i); });
    }
}

void JobSystem::stop() {
    if (!running_.exchange(false)) return;
    wakeup_.notify_all();
    for (auto& w : workers_) if (w->thread.joinable()) w->thread.join();
    workers_.clear();
    LOGI("JobSystem: остановлен");
}

u32 JobSystem::currentWorkerId() const { return tlsWorkerId_; }

void JobSystem::submit(JobFn fn, void* data) {
    u32 wid = tlsWorkerId_;
    Job j{fn, data, nullptr};
    if (wid != 0xFFFFFFFFu) {
        Worker& w = *workers_[wid];
        std::lock_guard lk(w.mtx);
        w.local.push_back(j);
    } else {
        std::lock_guard lk(globalMtx_);
        globalQueue_.push_back(j);
    }
    activeJobs_.fetch_add(1, std::memory_order_relaxed);
    wakeup_.notify_one();
}

void JobSystem::submit(Counter* c, JobFn fn, void* data) {
    c->add(1);
    u32 wid = tlsWorkerId_;
    Job j{fn, data, c};
    if (wid != 0xFFFFFFFFu) {
        Worker& w = *workers_[wid];
        std::lock_guard lk(w.mtx);
        w.local.push_back(j);
    } else {
        std::lock_guard lk(globalMtx_);
        globalQueue_.push_back(j);
    }
    activeJobs_.fetch_add(1, std::memory_order_relaxed);
    wakeup_.notify_one();
}

// Выполнить одну задачу с корректной обработкой counter
static void runJob(const JobSystem::Job& j) {
    j.fn(j.data);
    if (j.counter) j.counter->done();
}

bool JobSystem::popJob(Job& out, u32 workerId) {
    Worker& w = *workers_[workerId];
    {
        std::lock_guard lk(w.mtx);
        if (!w.local.empty()) {
            out = w.local.back();
            w.local.pop_back();
            return true;
        }
    }
    {
        std::lock_guard lk(globalMtx_);
        if (!globalQueue_.empty()) {
            out = globalQueue_.front();
            globalQueue_.pop_front();
            return true;
        }
    }
    return false;
}

bool JobSystem::trySteal(Job& out, u32 myId) {
    u32 n = (u32)workers_.size();
    if (n <= 1) return false;
    // Начинаем с "соседа", чтобы снизить контеншн
    u32 start = (myId + 1) % n;
    for (u32 i = 0; i < n; ++i) {
        u32 victim = (start + i) % n;
        if (victim == myId) continue;
        Worker& w = *workers_[victim];
        std::lock_guard lk(w.mtx);
        if (!w.local.empty()) {
            out = w.local.front();
            w.local.pop_front();
            return true;
        }
    }
    return false;
}

void JobSystem::workerLoop(u32 workerId) {
    tlsWorkerId_ = workerId;
    for (;;) {
        Job j;
        bool got = false;

        if (popJob(j, workerId)) got = true;
        else if (trySteal(j, workerId)) got = true;

        if (got) {
            runJob(j);
            activeJobs_.fetch_sub(1, std::memory_order_relaxed);
            continue;
        }

        // Ждём новую работу
        std::unique_lock lk(globalMtx_);
        wakeup_.wait_for(lk, std::chrono::milliseconds(2), [this]{
            if (!running_.load(std::memory_order_acquire)) return true;
            if (!globalQueue_.empty()) return true;
            for (auto& w : workers_) {
                std::lock_guard l(w->mtx);
                if (!w->local.empty()) return true;
            }
            return false;
        });
        if (!running_.load(std::memory_order_acquire) && globalQueue_.empty()) {
            // Дать шанс доработать очереди воркеров
            bool anyWork = false;
            for (auto& w : workers_) {
                std::lock_guard l(w->mtx);
                if (!w->local.empty()) { anyWork = true; break; }
            }
            if (!anyWork) return;
        }
    }
}

// parallelFor: разбиваем диапазон на чанки, каждую задачу прикрепляем к счётчику.
namespace {
struct RangeData {
    u32 begin, end;
    const std::function<void(u32,u32)>* fn;
};
void runRange(void* p) {
    auto* d = (RangeData*)p;
    (*d->fn)(d->begin, d->end);
}
}

void JobSystem::parallelFor(u32 count, u32 minChunk,
                            const std::function<void(u32,u32)>& fn)
{
    if (count == 0) return;
    if (minChunk == 0) minChunk = 1;

    // Без воркеров (до start() или после stop()) задачи никто не
    // разберёт, и c.wait() ниже стал бы вечным. Из самого воркера —
    // тоже нельзя: он ждал бы, не разбирая очередь, и несколько таких
    // ожиданий разом занимают все потоки. В обоих случаях — на месте.
    if (!running() || workers_.empty() || tlsWorkerId_ != 0xFFFFFFFFu) {
        fn(0, count);
        return;
    }

    u32 chunks = (count + minChunk - 1) / minChunk;
    u32 nWorkers = workerCount() + 1; // +1 — вызывающий поток тоже работает
    u32 perJob = std::max(1u, chunks / nWorkers);
    u32 step = perJob * minChunk;

    // Данные задач живут в векторе, чтобы не аллоцировать вручную.
    std::vector<RangeData> ranges;
    for (u32 b = 0; b < count; b += step) {
        u32 e = std::min(count, b + step);
        ranges.push_back({b, e, &fn});
    }

    // Счётчик увеличивает сам submit(Counter*, ...), поэтому здесь
    // add() делается только для диапазона, который выполняет
    // вызывающий поток.
    Counter c;
    for (usize i = 0; i + 1 < ranges.size(); ++i) {
        submit(&c, &runRange, &ranges[i]);
    }
    if (!ranges.empty()) {
        c.add(1);
        runRange(&ranges.back());
        c.done();
    }
    c.wait();
}

} // namespace jobs
