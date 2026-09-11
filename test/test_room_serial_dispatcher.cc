// RoomSerialDispatcher 并发正确性验证（独立可编译，不依赖 drogon）
//   1. 同房间任务严格按入队顺序执行（每线程自己的消息不得乱序）
//   2. 任务不丢失、不重复
//   3. 多房间并行时互不干扰，且排空后 Lane 回收
//   4. 队列超限时按房间限流并精确计数
//   5. 任务内重入派发同房间不会自锁死
#include "../ws_controllers/RoomSerialDispatcher.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                       \
    do                                                         \
    {                                                          \
        if (!(cond))                                           \
        {                                                      \
            std::printf("  [FAIL] %s\n", msg);                 \
            ++g_failures;                                      \
        }                                                      \
        else                                                   \
        {                                                      \
            std::printf("  [ ok ] %s\n", msg);                 \
        }                                                      \
    } while (0)

static void testSingleRoomOrdering()
{
    std::printf("test: 单房间多生产者 FIFO 顺序\n");
    RoomSerialDispatcher dispatcher;

    // 总量刻意控制在 kMaxRoomBacklog 之下，避免限流干扰顺序断言
    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;

    std::mutex outMutex;
    std::vector<std::pair<int, int>> executionOrder; // (threadId, seq)

    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t)
    {
        producers.emplace_back([&, t] {
            for (int s = 0; s < kPerThread; ++s)
            {
                dispatcher.dispatch("room_a", [&, t, s] {
                    std::lock_guard lock(outMutex);
                    executionOrder.emplace_back(t, s);
                });
            }
        });
    }
    for (auto& th : producers)
    {
        th.join();
    }

    CHECK(executionOrder.size() == static_cast<size_t>(kThreads) * kPerThread,
          "全部任务都被执行（无丢失、无重复）");
    CHECK(dispatcher.droppedCount() == 0, "无任务因积压被丢弃");

    // 关键断言：每个生产者的消息在全局执行序列中必须保持自增顺序
    std::vector<int> lastSeq(kThreads, -1);
    bool ordered = true;
    for (const auto& [t, s] : executionOrder)
    {
        if (s <= lastSeq[t])
        {
            ordered = false;
            break;
        }
        lastSeq[t] = s;
    }
    CHECK(ordered, "同房间内每个生产者的消息严格有序（无乱序）");
    CHECK(dispatcher.activeRooms() == 0, "房间排空后 Lane 被回收，无内存残留");
}

static void testMultiRoomParallel()
{
    std::printf("test: 多房间隔离\n");
    RoomSerialDispatcher dispatcher;

    constexpr int kRooms = 16;
    constexpr int kPerRoom = 500;

    std::vector<std::thread> producers;
    std::atomic<int> executed{0};
    for (int r = 0; r < kRooms; ++r)
    {
        producers.emplace_back([&, r] {
            for (int i = 0; i < kPerRoom; ++i)
            {
                dispatcher.dispatch("room_" + std::to_string(r), [&] {
                    executed.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }
    for (auto& th : producers)
    {
        th.join();
    }

    CHECK(executed.load() == kRooms * kPerRoom, "多房间任务全部执行");
    CHECK(dispatcher.droppedCount() == 0, "多房间未触发限流");
    CHECK(dispatcher.activeRooms() == 0, "所有房间 Lane 均已回收");
}

static void testBacklogLimit()
{
    std::printf("test: 积压限流\n");
    RoomSerialDispatcher dispatcher;

    // 用首个任务把房间钉住，让后续 dispatch 只能入队
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    std::atomic<int> executed{0};

    std::thread holder([&] {
        dispatcher.dispatch("busy", [&] {
            started.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            executed.fetch_add(1, std::memory_order_relaxed);
        });
    });

    while (!started.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    size_t accepted = 0;
    size_t rejected = 0;
    for (size_t i = 0; i < RoomSerialDispatcher::kMaxRoomBacklog + 100; ++i)
    {
        if (dispatcher.dispatch("busy", [&] { executed.fetch_add(1, std::memory_order_relaxed); }))
        {
            ++accepted;
        }
        else
        {
            ++rejected;
        }
    }

    CHECK(rejected > 0, "超出上限后新任务被拒绝（背压生效）");
    CHECK(accepted <= RoomSerialDispatcher::kMaxRoomBacklog,
          "单房间积压不超过 kMaxRoomBacklog，内存有界");
    CHECK(dispatcher.droppedCount() == rejected, "丢弃计数与实际拒绝数一致");

    release.store(true, std::memory_order_release);
    holder.join();
    CHECK(executed.load() == static_cast<int>(accepted) + 1, "被接纳的任务全部被执行完");
    CHECK(dispatcher.activeRooms() == 0, "限流后房间仍能正常排空回收");
}

static void testNoRecursionDeadlock()
{
    std::printf("test: 任务内再派发同房间（不可自锁死）\n");
    RoomSerialDispatcher dispatcher;

    std::atomic<int> executed{0};
    dispatcher.dispatch("nested", [&] {
        executed.fetch_add(1, std::memory_order_relaxed);
        // 抽干过程中重新入队同房间：必须立即返回而不是死锁
        dispatcher.dispatch("nested", [&] { executed.fetch_add(1, std::memory_order_relaxed); });
    });

    CHECK(executed.load() == 2, "同房间重入派发的任务仍被顺序执行");
    CHECK(dispatcher.activeRooms() == 0, "重入后房间状态一致");
}

int main()
{
    testSingleRoomOrdering();
    testMultiRoomParallel();
    testBacklogLimit();
    testNoRecursionDeadlock();

    std::printf("\n%s (failures=%d)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
