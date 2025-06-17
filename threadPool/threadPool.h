//
// Created by 神圣·凯莎 on 2022/6/18.
//

#ifndef THREADPOOL_H
#define THREADPOOL_H

#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>
#include <trantor/utils/Logger.h>

class ThreadPool
{
public:
    explicit ThreadPool(size_t threads);
    ~ThreadPool();

    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

    void setThreadCount(size_t newCount);

private:
    void workerThread();

    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
    size_t thread_count;
};

// Constructor
inline ThreadPool::ThreadPool(size_t threads)
    : stop(false), thread_count(threads) {
    for (size_t i = 0; i < threads; ++i) {
        workers.emplace_back(&ThreadPool::workerThread, this);
    }
}

// Worker thread function
inline void ThreadPool::workerThread() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            condition.wait(lock, [this] {
                return stop || !tasks.empty();
            });
            if (stop && tasks.empty())
                return;
            task = std::move(tasks.front());
            tasks.pop();
        }

        try {
            task();
        } catch (const std::exception& e) {
            LOG_ERROR << "ThreadPool task exception: " << e.what();
        }
    }
}

// Add new work item to the pool
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>> {
    using return_type = std::invoke_result_t<F, Args...>;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);

        if (stop)
            throw std::runtime_error("enqueue on stopped ThreadPool");

        tasks.emplace([task]() {
            try {
                (*task)();
            } catch (const std::exception& e) {
                LOG_ERROR << "ThreadPool task exception: " << e.what();
            }
        });
    }
    condition.notify_one();
    return res;
}

// Set thread count
inline void ThreadPool::setThreadCount(size_t newCount) {
    std::unique_lock<std::mutex> lock(queue_mutex);
    if (newCount > thread_count) {
        for (size_t i = thread_count; i < newCount; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this);
        }
    } else if (newCount < thread_count) {
        stop = true;
        condition.notify_all();
        for (size_t i = newCount; i < thread_count; ++i) {
            if (workers[i].joinable()) {
                workers[i].join();
            }
        }
        stop = false;
    }
    thread_count = newCount;
}

// Destructor
inline ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread& worker : workers)
        if (worker.joinable())
            worker.join();
}

#endif // THREADPOOL_H