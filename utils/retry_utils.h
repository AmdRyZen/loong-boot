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
