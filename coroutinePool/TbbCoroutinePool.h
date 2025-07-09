//
// Created by 神圣•凯莎 on 25-6-27.
//
#ifndef TBB_COROUTINEPOOL_H
#define TBB_COROUTINEPOOL_H

#pragma once
#include <functional>
#include <tbb/task_group.h>
#include <iostream>

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

    // 非模板版本，接受右值引用，避免复制
    void submit(std::function<void()>&& task) {
        tg_.run(std::move(task));
    }

    // 模板版本，直接转发给 TBB task_group
    template<typename TaskFunc>
    void submit(TaskFunc&& func) {
        tg_.run(std::forward<TaskFunc>(func));
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
};

#endif // TBB_COROUTINEPOOL_H
