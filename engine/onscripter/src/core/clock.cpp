/* -*- C++ -*-
 *
 *  clock.cpp - monotonic clock, sleep and engine timers
 *
 *  Copyright (c) 2026 NimbusVN contributors.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "clock.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace ons {

namespace {

typedef std::chrono::steady_clock SteadyClock;

SteadyClock::time_point start_time()
{
    static const SteadyClock::time_point t0 = SteadyClock::now();
    return t0;
}

struct TimerEntry {
    uint32_t id = 0;
    uint32_t interval = 0;
    SteadyClock::time_point due;
    TimerCallback callback = nullptr;
    void *param = nullptr;
    bool cancelled = false;
};

class TimerService {
public:
    static TimerService &instance()
    {
        static TimerService service;
        return service;
    }

    uint32_t add(uint32_t interval, TimerCallback cb, void *param)
    {
        if (!cb)
            return 0;
        if (interval == 0)
            interval = 1;

        std::lock_guard<std::mutex> lock(mutex_);
        TimerEntry entry;
        entry.id = next_id_++;
        if (next_id_ == 0)
            next_id_ = 1;
        entry.interval = interval;
        entry.due = SteadyClock::now() + std::chrono::milliseconds(interval);
        entry.callback = cb;
        entry.param = param;
        timers_.push_back(entry);

        ensure_thread();
        cv_.notify_all();
        return entry.id;
    }

    void remove(uint32_t id)
    {
        if (id == 0)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < timers_.size(); ++i) {
            if (timers_[i].id == id) {
                timers_[i].cancelled = true;
                timers_.erase(timers_.begin() + i);
                break;
            }
        }
        cv_.notify_all();
    }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            timers_.clear();
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            if (thread_.get_id() == std::this_thread::get_id())
                thread_.detach();
            else
                thread_.join();
        }
    }

private:
    TimerService() : next_id_(1), stopping_(false), started_(false) {}

    ~TimerService() { shutdown(); }

    TimerService(const TimerService &);
    TimerService &operator=(const TimerService &);

    void ensure_thread()
    {
        if (started_)
            return;
        started_ = true;
        thread_ = std::thread(&TimerService::run, this);
    }

    void run()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            if (stopping_)
                return;
            if (timers_.empty()) {
                cv_.wait(lock);
                continue;
            }

            /* Find the earliest deadline. */
            size_t best = 0;
            for (size_t i = 1; i < timers_.size(); ++i) {
                if (timers_[i].due < timers_[best].due)
                    best = i;
            }

            SteadyClock::time_point due = timers_[best].due;
            if (SteadyClock::now() < due) {
                cv_.wait_until(lock, due);
                continue;
            }

            TimerEntry fired = timers_[best];
            timers_.erase(timers_.begin() + best);
            lock.unlock();

            uint32_t next = fired.callback(fired.interval, fired.param);

            lock.lock();
            if (next != 0 && !stopping_) {
                /* The callback may have removed itself; only re-arm when the
                 * entry is still absent. */
                bool present = false;
                for (size_t i = 0; i < timers_.size(); ++i) {
                    if (timers_[i].id == fired.id) {
                        present = true;
                        break;
                    }
                }
                if (!present) {
                    fired.due = SteadyClock::now() + std::chrono::milliseconds(next);
                    fired.interval = next;
                    timers_.push_back(fired);
                }
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<TimerEntry> timers_;
    std::thread thread_;
    uint32_t next_id_;
    bool stopping_;
    bool started_;
};

} // namespace

uint32_t ticks_ms()
{
    SteadyClock::duration d = SteadyClock::now() - start_time();
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
}

void sleep_ms(uint32_t ms)
{
    if (ms == 0) {
        std::this_thread::yield();
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

uint32_t add_timer(uint32_t interval, TimerCallback callback, void *param)
{
    return TimerService::instance().add(interval, callback, param);
}

void remove_timer(uint32_t id)
{
    TimerService::instance().remove(id);
}

void shutdown_timers()
{
    TimerService::instance().shutdown();
}

} // namespace ons
