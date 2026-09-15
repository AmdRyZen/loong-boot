// 全局唯一消息 ID（SnowflakeId）的并发正确性验证。
//
// 为什么单独一个目标：它自带 int main()（不走 drogon 测试框架），
// 且【完全不依赖 drogon / 网络】—— 纯逻辑，秒级跑完。
//
// 覆盖：
//   1. 位段布局自洽（毫秒/实例槽位/序号三段能原样拆回来）
//   2. 单线程严格单调递增 + 无重复
//   3. 8 线程并发取号：总量正确、零重复、实例槽位一致
//   4. 同一毫秒内序号用满 4096 后能正确「借用下一毫秒」而不复用号
//   5. 毫秒字段单调不减（时间只会向前）
#include "../utils/SnowflakeId.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

using loong::util::SnowflakeId;

static int g_failures = 0;

#define CHECK(cond, msg)                       \
    do                                         \
    {                                          \
        if (!(cond))                           \
        {                                      \
            std::printf("  [FAIL] %s\n", msg); \
            ++g_failures;                      \
        }                                      \
        else                                   \
        {                                      \
            std::printf("  [ ok ] %s\n", msg); \
        }                                      \
    } while (0)

// 1. 位段布局
static void testBitLayout()
{
    std::printf("test: 位段布局\n");
    const uint64_t id = SnowflakeId::next();
    CHECK(SnowflakeId::slotOf(id) == SnowflakeId::instanceSlot(),
          "id 里的实例槽位 == instanceSlot()");
    CHECK(SnowflakeId::seqOf(id) <= SnowflakeId::kSeqMask, "序号字段不超过 12 bit");
    CHECK(SnowflakeId::msOf(id) > 0, "毫秒字段 > 0（epoch 取 2024-01-01）");
    CHECK((id >> 22) == SnowflakeId::msOf(id), "毫秒字段确实在高 41 位");

    // 三个字段拼回去必须等于原 id —— 否则提取函数与生成函数已经脱节
    const uint64_t rebuilt =
        (SnowflakeId::msOf(id) << 22) | (SnowflakeId::slotOf(id) << 12) | SnowflakeId::seqOf(id);
    CHECK(rebuilt == id, "三段字段拼回原 id");

    // 非法 LOONG_INSTANCE_ID 不应抛异常（静态量已求值，这里只验证不崩）
    std::printf("  实例槽位 = %llu\n", static_cast<unsigned long long>(SnowflakeId::instanceSlot()));
}

// 2. 单线程单调 + 无重复
static void testSingleThreadMonotonic()
{
    std::printf("test: 单线程单调性与唯一性（20 万条）\n");
    constexpr int kN = 200000;
    std::vector<uint64_t> ids;
    ids.reserve(kN);
    for (int i = 0; i < kN; ++i)
    {
        ids.push_back(SnowflakeId::next());
    }

    bool strictlyIncreasing = true;
    for (int i = 1; i < kN; ++i)
    {
        if (ids[i] <= ids[i - 1])
        {
            strictlyIncreasing = false;
            std::printf("  首个非递增位置 i=%d: %llu -> %llu\n", i,
                        static_cast<unsigned long long>(ids[i - 1]),
                        static_cast<unsigned long long>(ids[i]));
            break;
        }
    }
    CHECK(strictlyIncreasing, "20 万条严格单调递增");

    std::unordered_set<uint64_t> uniq(ids.begin(), ids.end());
    CHECK(uniq.size() == static_cast<size_t>(kN), "20 万条零重复");

    // 毫秒字段单调不减
    bool msMonotonic = true;
    for (int i = 1; i < kN; ++i)
    {
        if (SnowflakeId::msOf(ids[i]) < SnowflakeId::msOf(ids[i - 1]))
        {
            msMonotonic = false;
            break;
        }
    }
    CHECK(msMonotonic, "毫秒字段单调不减");
}

// 3. 并发取号
static void testConcurrent()
{
    std::printf("test: 8 线程并发取号（各 5 万条）\n");
    constexpr int kThreads = 8;
    constexpr int kPerThread = 50000;

    std::vector<std::vector<uint64_t>> perThread(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([t, &perThread] {
            auto& out = perThread[t];
            out.reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i)
            {
                out.push_back(SnowflakeId::next());
            }
        });
    }
    for (auto& th : threads)
    {
        th.join();
    }

    std::unordered_set<uint64_t> all;
    all.reserve(kThreads * kPerThread);
    bool slotConsistent = true;
    for (const auto& v : perThread)
    {
        for (const uint64_t id : v)
        {
            all.insert(id);
            if (SnowflakeId::slotOf(id) != SnowflakeId::instanceSlot())
            {
                slotConsistent = false;
            }
        }
    }
    CHECK(all.size() == static_cast<size_t>(kThreads * kPerThread),
          "8×5 万条并发取号零重复");
    CHECK(slotConsistent, "所有 id 的实例槽位一致");
}

// 4. 单毫秒序号用满 → 借用下一毫秒，不复用号
static void testSeqOverflowBorrowsNextMs()
{
    std::printf("test: 序号用满后借用下一毫秒\n");
    // 连续取 4096*3 条，必然跨过若干次「本毫秒号用满」。
    constexpr int kN = 4096 * 3;
    std::vector<uint64_t> ids;
    ids.reserve(kN);
    for (int i = 0; i < kN; ++i)
    {
        ids.push_back(SnowflakeId::next());
    }

    std::unordered_set<uint64_t> uniq(ids.begin(), ids.end());
    CHECK(uniq.size() == static_cast<size_t>(kN), "跨毫秒边界 12288 条零重复");

    // 序号在同一个毫秒内必须严格递增，且每个毫秒段内不出现回绕
    bool seqOk = true;
    for (int i = 1; i < kN; ++i)
    {
        const uint64_t prevMs = SnowflakeId::msOf(ids[i - 1]);
        const uint64_t curMs = SnowflakeId::msOf(ids[i]);
        if (curMs == prevMs && SnowflakeId::seqOf(ids[i]) <= SnowflakeId::seqOf(ids[i - 1]))
        {
            seqOk = false;
            break;
        }
        if (curMs < prevMs)
        {
            seqOk = false;
            break;
        }
    }
    CHECK(seqOk, "同毫秒内序号严格递增、毫秒不回退");
}

int main()
{
    std::printf("=== SnowflakeId 单元测试 ===\n\n");
    testBitLayout();
    std::printf("\n");
    testSingleThreadMonotonic();
    std::printf("\n");
    testConcurrent();
    std::printf("\n");
    testSeqOverflowBorrowsNextMs();
    std::printf("\n");

    if (g_failures == 0)
    {
        std::printf("ALL PASSED (failures=0)\n");
        return 0;
    }
    std::printf("FAILED (failures=%d)\n", g_failures);
    return 1;
}
