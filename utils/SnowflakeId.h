#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>  // getpid()
#endif

namespace loong::util
{

// ── 全局唯一、单实例内单调的消息 ID 生成器（snowflake 式，无锁） ───────────────
//
// 为什么需要它：原实现是
//     static std::atomic<uint64_t> seq{0};
//     return seq.fetch_add(1, relaxed) + 1;
// —— 纯【进程内】自增。两个后果：
//   ① 进程重启后 id 从 1 重新开始 ⇒ 新消息与历史消息 id 冲突，
//      客户端/Kafka 回放端都无法用 id 去重（`msg_vo.id` 于是成了摆设）；
//   ② 多实例部署时每个实例都从 1 开始 ⇒ 集群内 id 空间完全重叠。
//
// 位布局（64 bit）：
//   [63:22] 41 bit  自 kEpochMs 起的毫秒数（约 69 年不溢出）
//   [21:12] 10 bit  实例槽位（同一时刻最多 1024 个实例互不冲突）
//   [11: 0] 12 bit  同一毫秒内的序号（每毫秒最多 4096 条）
//
// 实例槽位来源：环境变量 `LOONG_INSTANCE_ID`（部署时显式分配，集群内保证唯一）；
// 未设置时退回 `getpid() & 0x3FF` —— 同机多实例天然不同，跨机有 1/1024 的碰撞
// 概率。对聊天脚手架足够，**生产集群应显式设置该变量**。
//
// 时钟回拨（NTP 校正 / 手动改时间）：不抛异常、不回退 id，而是【沿用上一毫秒并
// 继续自增】。宁可让 id 与真实时间短暂脱钩，也不能产生重复 id —— 重复比不准致命。
class SnowflakeId
{
public:
    static constexpr unsigned kSeqBits = 12;
    static constexpr unsigned kSlotBits = 10;
    static constexpr uint64_t kSeqMask = (1ULL << kSeqBits) - 1;    // 4095
    static constexpr uint64_t kSlotMask = (1ULL << kSlotBits) - 1;  // 1023

    // 生成下一个 id。线程安全、无锁、单实例内严格单调递增。
    static uint64_t next() noexcept
    {
        // state 打包 (毫秒 << kSeqBits) | 序号，用一次 CAS 同时完成
        // 「同毫秒内取号」与「推进毫秒」，避免两把尺子互相覆盖。
        static std::atomic<uint64_t> state{0};

        const uint64_t nowMs = currentMs();
        uint64_t old = state.load(std::memory_order_acquire);
        for (;;)
        {
            const uint64_t prevMs = old >> kSeqBits;
            uint64_t ms = std::max(prevMs, nowMs);  // 回拨 ⇒ 取 prevMs
            uint64_t seq;
            if (ms == prevMs)
            {
                seq = (old & kSeqMask) + 1;
                if (seq > kSeqMask)
                {
                    // 本毫秒 4096 个号已用满 ⇒ 借用下一毫秒。
                    // 宁可时间戳略超前，也不能阻塞或复用号。
                    ms += 1;
                    seq = 0;
                }
            }
            else
            {
                seq = 0;
            }

            const uint64_t nextState = (ms << kSeqBits) | seq;
            if (state.compare_exchange_weak(old,
                                            nextState,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire))
            {
                return (ms << (kSeqBits + kSlotBits)) | (instanceSlot() << kSeqBits) | seq;
            }
            // CAS 失败：old 已刷新为最新值，直接重试即可
        }
    }

    // ── 位段提取（诊断 / 客户端可用）────────────────────────────────────────
    static uint64_t msOf(uint64_t id) noexcept { return id >> (kSeqBits + kSlotBits); }
    static uint64_t slotOf(uint64_t id) noexcept { return (id >> kSeqBits) & kSlotMask; }
    static uint64_t seqOf(uint64_t id) noexcept { return id & kSeqMask; }

    // 本进程使用的实例槽位（只求值一次）。上报到指标里，
    // 便于确认「集群里两个实例是不是撞了同一个槽位」。
    static uint64_t instanceSlot() noexcept
    {
        static const uint64_t s = []() -> uint64_t {
            if (const char* v = std::getenv("LOONG_INSTANCE_ID"); v != nullptr && *v != '\0')
            {
                try
                {
                    return static_cast<uint64_t>(std::stoul(v)) & kSlotMask;
                }
                catch (...)
                {
                    // 非法值静默退回 pid：启动期不该因为一个环境变量写错就崩
                }
            }
#if defined(_WIN32)
            return 0;
#else
            return static_cast<uint64_t>(::getpid()) & kSlotMask;
#endif
        }();
        return s;
    }

private:
    static uint64_t currentMs() noexcept
    {
        // 自定义 epoch：2024-01-01T00:00:00Z。
        // 用自定义 epoch 而不是 1970，是为了把 41 bit 的可用年限从「到 2039 年」
        // 推到「到 2093 年」，代价只是多一个常量。
        constexpr int64_t kEpochMs = 1704067200000LL;
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        return now > kEpochMs ? static_cast<uint64_t>(now - kEpochMs) : 0ULL;
    }
};

}  // namespace loong::util
