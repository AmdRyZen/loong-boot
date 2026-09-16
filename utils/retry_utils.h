//
// Created by 神圣•凯莎 on 25-6-3.
//

#ifndef RETRY_UTILS_H
#define RETRY_UTILS_H

#pragma once
#include <chrono>
#include <coroutine>

using namespace drogon;


template<typename Duration>
auto delay(Duration duration)
{
    struct Awaiter
    {
        std::chrono::duration<double> seconds_;
        [[nodiscard]] bool await_ready() const noexcept { return seconds_.count() <= 0; }
        void await_suspend(std::coroutine_handle<> handle) const
        {
            HttpAppFramework::instance().getLoop()->runAfter(seconds_.count(), [handle]() {
                handle.resume();
            });
        }
        static void await_resume() noexcept {}
    };
    return Awaiter{std::chrono::duration_cast<std::chrono::duration<double>>(duration)};
}

template<typename Func, typename Duration>
requires requires(Func f) { { f() } -> std::same_as<Task<bool>>; }
Task<> retryWithDelayAsync(Func&& func,
                           const int maxRetries = 3,
                           Duration delayMs = Duration(100))
{
    static_assert(std::is_invocable_r_v<Task<bool>, Func>, "retryWithDelayAsync requires func() to return Task<bool>");

    for (int retry = 0; retry < maxRetries; ++retry)
    {
        if (co_await func())
        {
            co_return;
        }
        if (retry < maxRetries - 1)
        {
            co_await delay(delayMs);
        }
    }
    // Optional: final failure logging or callback can be added here.
}


// ⚠️ 阻塞式重试：内部用 std::this_thread::sleep_for 占着当前线程。
//
// 【不要在 TBB worker 或 drogon IO 线程上调用】。本工程的 TBB 并行度被
// aop/Application.h 的 tbb::global_control(max_allowed_parallelism, 核数) 锁死，
// 几个 worker 在 sleep 就等于池少几个执行槽；broker/下游抖动时所有 worker
// 一起进重试 ⇒ 池停止抽干 ⇒ activeTasks_ 涨过 32768 ⇒ submit 返回 false。
// 本工程曾因此在 Kafka 落库路径上踩过这个坑（见 ChatWebsocket.cc 里
// produceKafkaAsync 的说明，现已改为单次投递 + 失败即记终态）。
//
// 需要「等待后再试」请用上面的 retryWithDelayAsync（协程版，不占线程），
// 或把退避交给队列/定时器。
template<typename Func>
void retryWithSleep(Func&& func,
                    const int maxRetries = 3,
                    const std::chrono::milliseconds delayMs = std::chrono::milliseconds(100))
{
    static_assert(std::is_invocable_r_v<bool, Func>, "retryWithSleep requires func() to return bool");

    for (int retry = 0; retry < maxRetries; ++retry)
    {
        if (func())
        {
            return;
        }
        if (retry < maxRetries - 1)
        {
            std::this_thread::sleep_for(delayMs);
        }
    }
    // Optional: log or handle failure after final attempt.
}

#endif //RETRY_UTILS_H
