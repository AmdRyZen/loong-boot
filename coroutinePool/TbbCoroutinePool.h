//
// Created by 神圣•凯莎 on 25-6-27.
//
#ifndef TBB_COROUTINEPOOL_H
#define TBB_COROUTINEPOOL_H

#pragma once
#include <functional>
#include <tbb/task_group.h>
#include <iostream>
#include <atomic>

class TbbCoroutinePool {
public:
    static TbbCoroutinePool& instance() {
        static TbbCoroutinePool pool;
        return pool;
    }

    static void init(size_t /*numThreads*/) {
        // TBB 自动管理线程数，无需手动初始化
    }

    static void shutdown() {
        // TBB 自动管理线程池，无需手动关闭
    }

    void waitAll() {
        tg_.wait();
    }

    template<typename TaskFunc>
    bool submit(TaskFunc&& func) {
        // 当积压任务超过 32768 时进行背压限流，防止压测时无界内存暴涨
        if (activeTasks_.load(std::memory_order_relaxed) > 32768) {
            return false;
        }
        activeTasks_.fetch_add(1, std::memory_order_relaxed);
        tg_.run([this, f = std::forward<TaskFunc>(func)]() {
            try {
                f();
            } catch (...) {
            }
            activeTasks_.fetch_sub(1, std::memory_order_release);
        });
        return true;
    }

    size_t getActiveTasks() const {
        return activeTasks_.load(std::memory_order_relaxed);
    }

    ~TbbCoroutinePool() {
        try {
            waitAll();
        } catch (const std::exception& e) {
            std::cerr << "Exception in TbbCoroutinePool destructor: " << e.what() << std::endl;
        }
    }

    TbbCoroutinePool(const TbbCoroutinePool&) = delete;
    TbbCoroutinePool& operator=(const TbbCoroutinePool&) = delete;

private:
    TbbCoroutinePool() = default;
    tbb::task_group tg_;
    std::atomic<size_t> activeTasks_{0};
};

#endif // TBB_COROUTINEPOOL_H
