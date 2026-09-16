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
#include <cstdint>

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
                // ⚠️ 这里曾经是空的 catch(...)，异常被【完全静默】吞掉 ——
                // 无日志、无指标，池内任务抛异常时上层毫无感知。
                // 注意 ws_kafka_persist_dropped_total 覆盖不到这种情况：那个计数器
                // 记的是「submit 返回 false（池积压被拒）」，不是「任务执行时抛异常」。
                //
                // 只计数不打日志：这里跑在 TBB worker 上，异常频率与流量成正比，
                // 逐条打日志正是本项目反复踩过的热路径日志放大。
                // 计数由 PrometheusMetrics 在导出/定时拉取时读走
                // （tbb_coroutine_pool_task_exceptions_total），要定位具体异常
                // 再临时加打印。
                taskExceptions_.fetch_add(1, std::memory_order_relaxed);
            }
            activeTasks_.fetch_sub(1, std::memory_order_release);
        });
        return true;
    }

    size_t getActiveTasks() const {
        return activeTasks_.load(std::memory_order_relaxed);
    }

    // 池内任务抛出并被吞掉的异常总数（单调递增）。
    uint64_t getTaskExceptions() const {
        return taskExceptions_.load(std::memory_order_relaxed);
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
    // 被 catch(...) 吞掉的异常数。用原子量是因为 submit 可能被任意线程调用，
    // 而读取方（PrometheusMetrics 导出）在别的线程上。
    std::atomic<uint64_t> taskExceptions_{0};
};

#endif // TBB_COROUTINEPOOL_H
