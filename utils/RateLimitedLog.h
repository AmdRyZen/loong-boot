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
//     static loong::log::RateLimiter limiter{1000, "ws.bad_packet"};  // 最快 1 秒一条
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
//
// ── ⚠️ 尾数问题（2026-09-16 修）────────────────────────────────────────────
//
// 「下一次输出带上」这个机制有个洞：**突发结束之后就没有下一次输出了**。
// 客户端打了一波非法包、被限流压掉 178 条，然后安静下来 —— 那 178 条永远
// 等不到「下一次放行」，于是既没有日志、也没有指标，等于静默丢失。
// 实测：179 次丢弃只留下 1 行、且写的是 `1 occurrence(s)`。
//
// 修法：把限流器登记进一张全局表，由**周期性任务**（本工程是 ChatWebsocket 的
// 5 秒定时任务）调 `takeAllSuppressed()` 把尾数冲出来。见文件末尾。
//
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

namespace loong::log
{

class RateLimiter
{
  public:
    // label 是给「尾数冲刷」用的上下文标识（通常是字符串字面量，不持有所有权）。
    // 留空也能用，但冲刷时只能报成 unnamed —— 所以新加的限流器都应当带 label。
    explicit RateLimiter(int64_t intervalMs = 1000, std::string_view label = {}) noexcept
        : intervalNs_(intervalMs * 1000000LL), label_(label.data()), labelLen_(label.size())
    {
        registerSelf();
    }

    ~RateLimiter() noexcept
    {
        unregisterSelf();
    }

    // 注册表里存的是裸指针：拷贝/移动会让表里的指针指向失效对象，直接禁掉。
    // 本工程的用法全是「函数内 static」或「成员」，本来也不会被拷贝。
    RateLimiter(const RateLimiter&) = delete;
    RateLimiter& operator=(const RateLimiter&) = delete;
    RateLimiter(RateLimiter&&) = delete;
    RateLimiter& operator=(RateLimiter&&) = delete;

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

    std::string_view label() const noexcept
    {
        return {label_, labelLen_};
    }

    static int64_t nowNanos() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

  private:
    void registerSelf() noexcept;
    void unregisterSelf() noexcept;

    int64_t intervalNs_;
    const char* label_ = nullptr;
    size_t labelLen_ = 0;
    // 初值 0：首次调用 now - 0 远大于间隔，必然放行。
    std::atomic<int64_t> last_{0};
    std::atomic<uint64_t> suppressed_{0};
};

// ── 全局注册表（只为「尾数冲刷」存在，不在热路径上）──────────────────────────

namespace detail
{

struct LimiterRegistry
{
    std::mutex mtx;
    std::vector<RateLimiter*> limiters;
};

inline LimiterRegistry& registry() noexcept
{
    // ⚠️ 故意 new 出来且永不 delete。
    //
    // 限流器多为「函数内 static」或「类成员」，析构顺序不可控；如果注册表先析构，
    // 后析构的限流器会在 ~RateLimiter → unregisterSelf 里访问已释放的 vector。
    // 本工程已经被「静态析构顺序」坑过一次（见退出段错误那节），这里直接绕过。
    static LimiterRegistry* r = new LimiterRegistry();
    return *r;
}

} // namespace detail

inline void RateLimiter::registerSelf() noexcept
{
    auto& reg = detail::registry();
    std::lock_guard lock(reg.mtx);
    reg.limiters.push_back(this);
}

inline void RateLimiter::unregisterSelf() noexcept
{
    auto& reg = detail::registry();
    std::lock_guard lock(reg.mtx);
    for (auto it = reg.limiters.begin(); it != reg.limiters.end(); ++it)
    {
        if (*it == this)
        {
            reg.limiters.erase(it);
            return;
        }
    }
}

// 一次「尾数」快照：某个限流器积压了多少条还没被任何一行日志带走。
struct SuppressedTail
{
    std::string_view label;
    uint64_t count = 0;
};

// 取出所有限流器当前的尾数并清零，**只返回非 0 的**。
// 由周期性任务调用；调用方负责把每一条输出成一行日志。
//
// 为什么要调用方输出而不是本函数自己 log：本头文件刻意不依赖 drogon/日志设施
// （它同时被 utils/checkloginUtils.h 这类底层头使用）。
inline std::vector<SuppressedTail> takeAllSuppressed()
{
    std::vector<SuppressedTail> out;
    auto& reg = detail::registry();
    std::lock_guard lock(reg.mtx);
    out.reserve(reg.limiters.size());
    for (RateLimiter* p : reg.limiters)
    {
        if (const uint64_t n = p->takeSuppressed(); n != 0)
        {
            const auto lb = p->label();
            out.push_back(SuppressedTail{lb.empty() ? std::string_view{"unnamed"} : lb, n});
        }
    }
    return out;
}

} // namespace loong::log
