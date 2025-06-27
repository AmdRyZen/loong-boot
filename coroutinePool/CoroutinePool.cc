//
// Created by 神圣•凯莎 on 25-6-27.
//

#include "CoroutinePool.h"

CoroutinePool& CoroutinePool::instance() {
    static CoroutinePool pool;
    return pool;
}

void CoroutinePool::init(const size_t numThreads) {
    stopping_ = false;
    for (size_t i = 0; i < numThreads; ++i) {
        workers_.emplace_back([this]() { runLoop(); });
    }
}

void CoroutinePool::shutdown() {
    stopping_ = true;
    condVar_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

void CoroutinePool::waitAll() {
    while (true) {
        std::unique_lock<std::mutex> lock(queueMutex_);
        if (taskQueue_.empty()) break;
        // 等待任务队列有变化或超时，防止死循环空转
        condVar_.wait_for(lock, std::chrono::milliseconds(10));
    }
}

void CoroutinePool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        taskQueue_.push(std::move(task));
    }
    condVar_.notify_one();  // 唤醒等待中的线程和 waitAll()
}

CoroutinePool::~CoroutinePool() {
    if (!stopping_) {  // 防止重复关闭
        waitAll();
        shutdown();
    }
}

void CoroutinePool::runLoop() {
    while (!stopping_) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            condVar_.wait(lock, [this]() {
                return stopping_ || !taskQueue_.empty();
            });
            if (stopping_ && taskQueue_.empty()) break;
            task = std::move(taskQueue_.front());
            taskQueue_.pop();
        }
        task();
        condVar_.notify_one();  // 任务执行完毕后通知等待线程
    }
}
