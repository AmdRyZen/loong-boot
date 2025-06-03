//
// Created by 神圣•凯莎 on 25-6-3.
//

#ifndef RETRY_UTILS_H
#define RETRY_UTILS_H

#pragma once
#include <chrono>
#include <coroutine>

using namespace drogon;


inline auto delay(const std::chrono::milliseconds duration)
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

template<typename Func>
Task<> retryWithDelayAsync(Func&& func,
                           const int maxRetries = 3,
                                  std::chrono::milliseconds delayMs = std::chrono::milliseconds(100))
{
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
}


template<typename Func>
void retryWithSleep(Func&& func,
                    const int maxRetries = 3,
                    const std::chrono::milliseconds delayMs = std::chrono::milliseconds(100))
{
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
}

#endif //RETRY_UTILS_H
