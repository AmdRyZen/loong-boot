#pragma once
//
// 客户端可控频率的日志限流器。
//
// 问题：有些 ERROR 日志的**触发频率由客户端决定** —— 乱发 token、狂发非法 JSON
// 都能让服务端每秒写几十万行日志、把磁盘写满。这与「过载路径回一帧通知」
// 属于同一类放大漏洞（见 RoomRegistry 与 ChatWebsocket 的过载处理）。
//
// 这类日志**不能静默**（它们是真错误，是排查依据），但**必须限流**。
//
// 用法：
//     static loong::log::RateLimiter limiter{1000};   // 最快 1 秒一条
//     if (limiter.allow())
//     {
//         LOG_ERROR << "..." << " (suppressed " << limiter.takeSuppressed()
//                   << " since last log)";
//     }
//
// `takeSuppressed()` 返回「自上次放行以来被抑制的次数」，并在读取后清零 ——
// 所以信息不丢：被压掉的量会在下一次输出里带上。
//
// 注意：必须在 `allow()` 返回 true 之后调用 `takeSuppressed()`，
// 否则会把还没输出的计数提前取走。
#include <atomic>
#include <chrono>
#include <cstdint>

namespace loong::log
{

class RateLimiter
{
  public:
    explicit RateLimiter(int64_t intervalMs = 1000) noexcept
        : intervalNs_(intervalMs * 1000000LL)
    {
    }

    // CAS 语义：只有成功推进时间戳的那一方返回 true，天然去重，无需额外锁。
    bool allow() noexcept
    {
        const int64_t now = nowNanos();
        int64_t last = last_.load(std::memory_order_relaxed);
        if (now - last < intervalNs_)
        {
            suppressed_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (last_.compare_exchange_strong(last, now, std::memory_order_relaxed))
        {
            return true;
        }
        // 竞争失败：别的线程刚抢到放行权，本次计入被抑制
        suppressed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // 读取并清零「自上次放行以来被抑制的次数」。
    uint64_t takeSuppressed() noexcept
    {
        return suppressed_.exchange(0, std::memory_order_relaxed);
    }

  private:
    static int64_t nowNanos() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    int64_t intervalNs_;
    // 初值 0：首次调用 now - 0 远大于间隔，必然放行。
    std::atomic<int64_t> last_{0};
    std::atomic<uint64_t> suppressed_{0};
};

} // namespace loong::log
