//
// Created by 神圣•凯莎 on 25-6-27.
//

#ifndef COROUTINEPOOL_H
#define COROUTINEPOOL_H

#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <functional>
#include <atomic>

class CoroutinePool {
public:
    static CoroutinePool& instance();

    void init(size_t numThreads = std::thread::hardware_concurrency());
    void shutdown();
    void waitAll();
    void submit(std::function<void()> task);

    template<typename TaskFunc>
    void submit(TaskFunc&& func) {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            taskQueue_.emplace(std::forward<TaskFunc>(func));
        }
        condVar_.notify_one();
    }

private:
    CoroutinePool() = default;
    ~CoroutinePool();

    void runLoop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> taskQueue_;
    std::mutex queueMutex_;
    std::condition_variable condVar_;
    std::atomic<bool> stopping_{false};
};

#endif //COROUTINEPOOL_H
