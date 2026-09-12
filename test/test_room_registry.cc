// RoomRegistry 并发正确性 + 并行度验证（独立可编译，不依赖 drogon 运行时）
//   1. 严格保序：同一房间内【所有订阅者看到的消息序列必须完全一致】——
//      这是「分片并行扇出不得破坏顺序」的最强断言。
//      分别覆盖「线程池并行扇出」与「内联扇出」两种模式。
//   2. 真实并行：多分片的 send 必须落在多个不同线程上，否则多核扇出等于没生效。
//   3. 背压：分片积压达上限时 publish 返回 false 且计数精确，不静默丢。
//   4. 并发订阅/退订与发布并发进行不崩、不丢唤醒，全部退订后房间被回收。
//   5. 空房间回收 / 无订阅者发布 / 退订幂等。
//   6. 直投与排队混用时的保序（7c 回归：本线程直投不得抢跑已排队的批次）。
//   7. 房间句柄 hint 快路径：命中投递、失效回退、单非空分片快路径不漏投。
#include "../ws_controllers/RoomRegistry.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <algorithm>
#include <functional>
#include <utility>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
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

// 连接只需满足「可 bool 判空 + 有 send(std::string_view)」，用统一接口串起来
struct IConn
{
    virtual ~IConn() = default;
    virtual void send(std::string_view) = 0;
};

// 普通 mock：记录收到的全部 payload（按到达顺序）
struct MockConn : IConn
{
    std::mutex m;
    std::vector<std::string> received;

    void send(std::string_view sv) override
    {
        std::lock_guard lock(m);
        received.emplace_back(sv);
    }
};

using MockPtr = std::shared_ptr<IConn>;
using Reg = RoomRegistryT<MockPtr>;

// 加锁读快照：扇出线程池模式下 MockConn::received 会被池线程写入，
// 直接读属于测试自身的数据竞争（TSan 下会报），统一走这个入口。
static std::vector<std::string> snapshotOf(const std::shared_ptr<MockConn>& c)
{
    std::lock_guard lock(c->m);
    return c->received;
}

static std::string encode(int producer, int seq)
{
    return std::to_string(static_cast<uint64_t>(producer) * 1000000ull +
                          static_cast<uint64_t>(seq));
}

static std::pair<int, int> decode(const std::string& s)
{
    const uint64_t v = std::stoull(s);
    return {static_cast<int>(v / 1000000ull), static_cast<int>(v % 1000000ull)};
}

static bool waitUntil(const std::function<bool()>& pred, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

// ---------------------------------------------------------------------------
// 1. 严格保序
// ---------------------------------------------------------------------------
static void testStrictOrdering(size_t inlineMaxSubs, const char* modeName)
{
    std::printf("test: 严格保序 [%s]\n", modeName);

    Reg::Options opt;
    opt.maxShards = 8;
    opt.fanoutThreads = 8;
    opt.inlineMaxSubs = inlineMaxSubs;
    Reg reg(opt);

    constexpr int kSubs = 100;
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 200;
    constexpr size_t kTotal = static_cast<size_t>(kProducers) * kPerProducer;

    std::vector<std::shared_ptr<MockConn>> conns;
    for (int i = 0; i < kSubs; ++i)
    {
        auto c = std::make_shared<MockConn>();
        conns.push_back(c);
        reg.subscribe("room", c);
    }
    CHECK(reg.subscribersIn("room") == static_cast<size_t>(kSubs), "订阅者数量正确");
    CHECK(reg.activeShards() == 8, "100 个订阅者铺满 8 个分片");

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p)
    {
        producers.emplace_back([&reg, p] {
            for (int s = 0; s < kPerProducer; ++s)
            {
                reg.publish("room", encode(p, s));
            }
        });
    }
    for (auto& t : producers)
    {
        t.join();
    }

    // 注意：必须等【全部】订阅者都收满，不能只等第一个 —— 各分片排空进度不同步，
    // 只等一个会误判成「丢失」（这是测试自身的竞态，不是实现缺陷）。
    const size_t expectedTotal = kSubs * kTotal;
    auto totalReceived = [&] {
        size_t n = 0;
        for (const auto& c : conns)
        {
            std::lock_guard lock(c->m);
            n += c->received.size();
        }
        return n;
    };
    const bool drained = waitUntil([&] { return totalReceived() >= expectedTotal; }, 8000);
    CHECK(drained, "扇出在超时前完成");
    if (!drained)
    {
        size_t mn = SIZE_MAX;
        size_t mx = 0;
        for (const auto& c : conns)
        {
            mn = std::min(mn, c->received.size());
            mx = std::max(mx, c->received.size());
        }
        std::printf("       [诊断] 期望=%zu 最小=%zu 最大=%zu dropped=%zu\n",
                    kTotal,
                    mn,
                    mx,
                    reg.droppedCount());
    }

    // 取锁快照，避免与尚未收尾的扇出线程竞争
    std::vector<std::vector<std::string>> snapshots;
    snapshots.reserve(conns.size());
    for (const auto& c : conns)
    {
        std::lock_guard lock(c->m);
        snapshots.push_back(c->received);
    }

    bool allSameSize = true;
    for (const auto& s : snapshots)
    {
        if (s.size() != kTotal)
        {
            allSameSize = false;
            break;
        }
    }
    CHECK(allSameSize, "每个订阅者都收到了全部消息（无丢失、无重复）");

    // 最强断言：所有订阅者看到的消息序列完全一致
    bool identical = true;
    const auto& ref = snapshots.front();
    for (size_t i = 1; i < snapshots.size() && identical; ++i)
    {
        if (snapshots[i] != ref)
        {
            identical = false;
        }
    }
    CHECK(identical, "所有订阅者看到的消息顺序完全一致（分片并行未破坏保序）");

    // 每个生产者自己的消息在全局序列中必须严格递增
    std::vector<int> lastSeq(kProducers, -1);
    bool perProducerOrdered = true;
    for (const auto& payload : ref)
    {
        const auto [p, s] = decode(payload);
        if (s <= lastSeq[p])
        {
            perProducerOrdered = false;
            break;
        }
        lastSeq[p] = s;
    }
    CHECK(perProducerOrdered, "同一生产者发出的消息在全局序列中严格递增（FIFO）");

    // 无重复：全部 payload 应互不相同
    std::set<std::string> distinct(ref.begin(), ref.end());
    CHECK(distinct.size() == kTotal, "消息集合无重复");
}

// ---------------------------------------------------------------------------
// 2. 真实并行度
// ---------------------------------------------------------------------------
struct BarrierConn : IConn
{
    std::mutex* m{nullptr};
    std::condition_variable* cv{nullptr};
    std::set<std::thread::id>* tids{nullptr};
    int* arrived{nullptr};
    int expected{0};

    void send(std::string_view) override
    {
        std::unique_lock lock(*m);
        tids->insert(std::this_thread::get_id());
        ++(*arrived);
        cv->notify_all();
        cv->wait_for(lock, std::chrono::milliseconds(3000),
                     [this] { return *arrived >= expected; });
    }
};

static void testRealParallelism()
{
    std::printf("test: 分片真实并行（send 落在多个线程）\n");

    Reg::Options opt;
    opt.maxShards = 8;
    opt.subsPerShard = 1; // 让 8 个订阅者直接铺满 8 个分片
    opt.fanoutThreads = 8;
    opt.inlineMaxSubs = 0; // 强制走线程池
    Reg reg(opt);

    constexpr int kSubs = 8;
    std::mutex m;
    std::condition_variable cv;
    std::set<std::thread::id> tids;
    int arrived = 0;

    std::vector<std::shared_ptr<BarrierConn>> conns;
    for (int i = 0; i < kSubs; ++i)
    {
        auto c = std::make_shared<BarrierConn>();
        c->m = &m;
        c->cv = &cv;
        c->tids = &tids;
        c->arrived = &arrived;
        c->expected = kSubs;
        conns.push_back(c);
        reg.subscribe("parallel", c);
    }
    CHECK(reg.activeShards() == static_cast<size_t>(kSubs), "8 个订阅者各自独占一个分片");

    const auto start = std::chrono::steady_clock::now();
    reg.publish("parallel", std::string("x"));

    {
        std::unique_lock lock(m);
        cv.wait_for(lock, std::chrono::seconds(3), [&] { return arrived >= kSubs; });
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();

    CHECK(arrived == kSubs, "8 个分片全部完成扇出（若串行会因屏障互相等待而超时）");
    CHECK(tids.size() >= 2, "扇出确实落在多个线程上（多核并行生效）");
    CHECK(elapsedMs < 1000, "并行扇出在 1 秒内完成");
    std::printf("       实际参与线程数 = %zu, 耗时 = %lldms\n",
                tids.size(),
                static_cast<long long>(elapsedMs));
}

// ---------------------------------------------------------------------------
// 3. 背压
// ---------------------------------------------------------------------------
struct BlockingConn : IConn
{
    std::atomic<bool> blocked{true};
    std::atomic<int> sent{0};

    void send(std::string_view) override
    {
        while (blocked.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        sent.fetch_add(1, std::memory_order_relaxed);
    }
};

static void testBackpressure()
{
    std::printf("test: 背压限流（不静默丢消息）\n");

    constexpr size_t kBacklog = 64;

    Reg::Options opt;
    opt.maxShards = 1;
    opt.fanoutThreads = 2;
    opt.inlineMaxSubs = 0;
    opt.backlogPerShard = kBacklog;
    Reg reg(opt);

    auto conn = std::make_shared<BlockingConn>();
    reg.subscribe("busy", conn);

    // 第一条会被 worker 取走并阻塞在 send 里，其余只能排队
    size_t accepted = 0;
    size_t rejected = 0;
    for (size_t i = 0; i < kBacklog + 300; ++i)
    {
        if (reg.publish("busy", std::string("m")))
        {
            ++accepted;
        }
        else
        {
            ++rejected;
        }
    }

    CHECK(rejected > 0, "超出上限后 publish 返回 false（背压生效）");
    CHECK(accepted <= kBacklog + 2, "单分片积压不超过 backlogPerShard，内存有界");
    CHECK(reg.droppedCount() == rejected, "丢弃计数与实际拒绝数一致");

    conn->blocked.store(false, std::memory_order_release);
    const bool done = waitUntil(
        [&] { return conn->sent.load(std::memory_order_relaxed) >= static_cast<int>(accepted); },
        5000);
    CHECK(done, "解除阻塞后被接纳的消息全部投递完毕");
    std::printf("       accepted=%zu rejected=%zu sent=%d\n",
                accepted,
                rejected,
                conn->sent.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// 4. 并发订阅 / 退订 / 发布
// ---------------------------------------------------------------------------
static void testConcurrentChurn()
{
    std::printf("test: 并发订阅/退订/发布\n");

    Reg::Options opt;
    opt.maxShards = 4;
    opt.fanoutThreads = 4;
    opt.inlineMaxSubs = 0;
    Reg reg(opt);

    std::atomic<bool> stop{false};
    std::atomic<int> published{0};

    std::vector<std::thread> churn;
    for (int t = 0; t < 4; ++t)
    {
        churn.emplace_back([&reg, &stop] {
            for (int round = 0; round < 300; ++round)
            {
                auto c = std::make_shared<MockConn>();
                const auto id = reg.subscribe("churn", c);
                reg.publish("churn", std::string("hello"));
                reg.unsubscribe("churn", id);
            }
        });
    }
    std::vector<std::thread> producers;
    for (int t = 0; t < 2; ++t)
    {
        producers.emplace_back([&reg, &stop, &published] {
            while (!stop.load(std::memory_order_acquire))
            {
                reg.publish("churn", std::string("bg"));
                published.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : churn)
    {
        t.join();
    }
    stop.store(true, std::memory_order_release);
    for (auto& t : producers)
    {
        t.join();
    }

    CHECK(published.load() > 0, "并发期间发布正常进行");
    CHECK(reg.activeRooms() == 0, "全部退订后房间被回收（无孤儿房间）");
    CHECK(reg.activeShards() == 0, "全部退订后分片被回收");
}

// ---------------------------------------------------------------------------
// 5. 边界行为
// ---------------------------------------------------------------------------
static void testEdgeCases()
{
    std::printf("test: 边界行为\n");

    Reg reg;

    CHECK(reg.publish("nobody", std::string("x")), "向不存在的房间发布返回成功（无订阅者，静默丢弃）");

    auto c = std::make_shared<MockConn>();
    const auto id = reg.subscribe("r", c);
    CHECK(reg.subscribersIn("r") == 1, "订阅后计数为 1");
    CHECK(reg.activeRooms() == 1, "房间已创建");

    reg.unsubscribe("r", id);
    reg.unsubscribe("r", id); // 幂等
    CHECK(reg.subscribersIn("r") == 0, "退订后计数为 0");
    CHECK(reg.activeRooms() == 0, "空房间被回收");

    reg.unsubscribe("never_existed", 12345);
    CHECK(true, "对不存在的房间退订不崩溃");

    // 空连接（nullptr）不应导致崩溃
    const auto id2 = reg.subscribe("n", MockPtr{});
    reg.publish("n", std::string("x"));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    reg.unsubscribe("n", id2);
    CHECK(true, "订阅了空连接也不崩溃");
}

// ---------------------------------------------------------------------------
// 6. 分片阶梯增长（reshape）时的保序
// ---------------------------------------------------------------------------
static void testReshapeOrdering()
{
    std::printf("test: 分片阶梯增长（reshape）时的保序\n");

    Reg::Options opt;
    opt.maxShards = 8;
    opt.subsPerShard = 4; // 阈值 4/8/16/32，快速触发多次 reshape
    opt.fanoutThreads = 4;
    opt.inlineMaxSubs = 0;
    Reg reg(opt);

    constexpr int kSubs = 60;
    constexpr int kMsgs = 4000;

    std::vector<std::shared_ptr<MockConn>> conns;
    std::atomic<bool> publishing{true};

    // 发布线程：序号严格递增
    std::thread producer([&] {
        for (int i = 0; i < kMsgs; ++i)
        {
            reg.publish("grow", std::to_string(i));
        }
        publishing.store(false, std::memory_order_release);
    });

    // 订阅线程：逐个加入，反复跨越分片数阶梯（触发 reshapeLocked）
    std::thread adder([&] {
        for (int i = 0; i < kSubs; ++i)
        {
            auto c = std::make_shared<MockConn>();
            conns.push_back(c);
            reg.subscribe("grow", c);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    producer.join();
    adder.join();

    // 等扇出排空：总量连续两次采样不变即认为已静止
    auto totalNow = [&] {
        size_t n = 0;
        for (const auto& c : conns)
        {
            std::lock_guard lock(c->m);
            n += c->received.size();
        }
        return n;
    };
    size_t last = totalNow();
    for (int i = 0; i < 200; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const size_t cur = totalNow();
        if (cur == last && cur > 0)
        {
            break;
        }
        last = cur;
    }

    // 核心断言：每个订阅者看到的序号必须严格递增（reshape 不得让新旧分片消息交错乱序）
    bool allMonotonic = true;
    size_t totalReceived = 0;
    for (const auto& c : conns)
    {
        std::lock_guard lock(c->m);
        totalReceived += c->received.size();
        long long prev = -1;
        for (const auto& s : c->received)
        {
            const long long v = std::stoll(s);
            if (v <= prev)
            {
                allMonotonic = false;
                std::printf("       [诊断] 乱序: %lld 出现在 %lld 之后\n", v, prev);
                break;
            }
            prev = v;
        }
        if (!allMonotonic)
        {
            break;
        }
    }
    CHECK(allMonotonic, "reshape 过程中每个订阅者看到的序号严格递增（无乱序）");
    CHECK(reg.activeShards() == 8, "60 个订阅者最终铺满 8 个分片");
    CHECK(reg.subscribersIn("grow") == kSubs, "订阅者数量正确");
    std::printf("       共投递 %zu 条（订阅者陆续加入，各自收到的起点不同）\n", totalReceived);
}

// ---------------------------------------------------------------------------
// 7. 按 IO 线程分组投递（A1）
// ---------------------------------------------------------------------------
// 模拟一个事件循环：记录被唤醒（queueInLoop）的次数，把批次存起来等「loop 线程」
// 稍后执行。isCurrentThread() 恒为 false，从而强制走「跨线程分组」分支。
struct LoopState
{
    // 该 loop 的身份。DynamicLoopHandle 用它判断「当前是否就在这个 loop 的线程上」，
    // 从而支持「同一 loop 既可能被直投、也可能被排队」的真实场景（见 7c）。
    int id{-1};
    std::mutex m;
    std::vector<std::function<void()>> pending;
    std::atomic<int> wakeups{0};
};

struct MockLoopHandle
{
    LoopState* st{nullptr};

    bool valid() const noexcept
    {
        return st != nullptr;
    }
    // 恒 false → 永远走「跨线程」分支；st 为空时退化为直投（不该发生）
    bool isCurrentThread() const noexcept
    {
        return st == nullptr;
    }
    template <typename F>
    void dispatch(F&& f) const
    {
        st->wakeups.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(st->m);
        st->pending.emplace_back(std::forward<F>(f));
    }
    bool operator==(const MockLoopHandle& o) const noexcept
    {
        return st == o.st;
    }
};

// 恒「就在本线程」的句柄 → 走直投分支
struct AlwaysCurrentHandle
{
    bool valid() const noexcept
    {
        return false;
    }
    bool isCurrentThread() const noexcept
    {
        return true;
    }
    template <typename F>
    void dispatch(F&& f) const
    {
        std::forward<F>(f)();
    }
    bool operator==(const AlwaysCurrentHandle&) const noexcept
    {
        return true;
    }
};

using RegGrouped = RoomRegistryT<MockPtr, MockLoopHandle>;
using RegDirect = RoomRegistryT<MockPtr, AlwaysCurrentHandle>;

// 「动态」句柄：isCurrentThread() 取决于【当前正在扮演哪个 loop 的线程】
//（tlsCurrentLoop）。这是唯一能覆盖「同一 loop 既被直投、又被排队」的句柄类型
// —— MockLoopHandle 恒 false、AlwaysCurrentHandle 恒 true，两者从不混用，
// 所以那条路径在 7c 之前完全没有被测试触及。
static thread_local int tlsCurrentLoop = -1;

struct DynamicLoopHandle
{
    LoopState* st{nullptr};

    bool valid() const noexcept
    {
        return st != nullptr;
    }
    bool isCurrentThread() const noexcept
    {
        return st != nullptr && st->id == tlsCurrentLoop;
    }
    template <typename F>
    void dispatch(F&& f) const
    {
        st->wakeups.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(st->m);
        st->pending.emplace_back(std::forward<F>(f));
    }
    bool operator==(const DynamicLoopHandle& o) const noexcept
    {
        return st == o.st;
    }
};

using RegDynamic = RoomRegistryT<MockPtr, DynamicLoopHandle>;

static void testLoopGrouping()
{
    std::printf("test: 按 IO 线程分组投递（A1）\n");

    constexpr int kLoops = 4;
    constexpr int kPerLoop = 10;
    constexpr int kMsgs = 50;
    constexpr int kSubs = kLoops * kPerLoop;

    // ---- 7a. 跨线程分组：唤醒次数必须由 O(订阅者) 降到 O(loop) ----
    {
        RegGrouped::Options opt;
        opt.maxShards = 1;     // 单分片：把变量收敛到「分组」这一件事上
        opt.fanoutThreads = 0; // 内联扇出，发布线程直接投递
        RegGrouped reg(opt);

        std::vector<std::unique_ptr<LoopState>> loops;
        for (int i = 0; i < kLoops; ++i)
        {
            loops.push_back(std::make_unique<LoopState>());
        }

        std::vector<std::shared_ptr<MockConn>> conns;
        for (int i = 0; i < kSubs; ++i)
        {
            auto c = std::make_shared<MockConn>();
            conns.push_back(c);
            reg.subscribe("grp", c, MockLoopHandle{loops[i % kLoops].get()});
        }

        for (int i = 0; i < kMsgs; ++i)
        {
            reg.publish("grp", std::to_string(i));
        }

        int totalWakeups = 0;
        for (const auto& l : loops)
        {
            totalWakeups += l->wakeups.load();
        }
        CHECK(totalWakeups == kLoops * kMsgs,
              "唤醒次数 = loop 数 × 消息数（而非订阅者数 × 消息数）");
        CHECK(reg.crossThreadBatches() == static_cast<size_t>(kLoops * kMsgs),
              "跨线程批次数精确等于 loop 数 × 消息数");
        CHECK(reg.inLoopDeliveries() == 0, "全跨线程时本线程直投计数为 0");

        // 扮演「各 loop 的线程」执行批次
        for (const auto& l : loops)
        {
            std::vector<std::function<void()>> take;
            {
                std::lock_guard lock(l->m);
                take.swap(l->pending);
            }
            for (auto& f : take)
            {
                f();
            }
        }

        bool allCount = true;
        bool allOrdered = true;
        for (const auto& c : conns)
        {
            std::lock_guard lock(c->m);
            if (c->received.size() != static_cast<size_t>(kMsgs))
            {
                allCount = false;
            }
            for (size_t i = 0; i < c->received.size(); ++i)
            {
                if (c->received[i] != std::to_string(i))
                {
                    allOrdered = false;
                    break;
                }
            }
        }
        CHECK(allCount, "分组投递不丢消息（每个订阅者都收满）");
        CHECK(allOrdered, "分组投递不破坏保序（每个订阅者顺序与发布顺序一致）");
        std::printf("       唤醒数=%d（若逐份跨线程投递应为 %d）\n",
                    totalWakeups,
                    kSubs * kMsgs);
    }

    // ---- 7b. 直投分支：命中本线程时不应产生任何唤醒 ----
    {
        RegDirect::Options opt;
        opt.maxShards = 1;
        opt.fanoutThreads = 0;
        RegDirect reg(opt);

        std::vector<std::shared_ptr<MockConn>> conns;
        for (int i = 0; i < 5; ++i)
        {
            auto c = std::make_shared<MockConn>();
            conns.push_back(c);
            reg.subscribe("direct", c, AlwaysCurrentHandle{});
        }
        for (int i = 0; i < 20; ++i)
        {
            reg.publish("direct", std::to_string(i));
        }

        CHECK(reg.crossThreadBatches() == 0, "命中本线程时不产生跨线程批次");
        CHECK(reg.inLoopDeliveries() == 5 * 20, "本线程直投计数 = 订阅者数 × 消息数");

        bool allOk = true;
        for (const auto& c : conns)
        {
            std::lock_guard lock(c->m);
            if (c->received.size() != 20)
            {
                allOk = false;
            }
        }
        CHECK(allOk, "直投分支全部送达");
    }
}

// ---- 7c. 直投与排队混用：保序回归（2026-09-12 修复的真实缺陷）----
//
// 缺陷机制（原实现）：fanOutToSnapshot 对「命中本线程」的组直接 send、对「跨线程」的组
// 走 queueInLoop；而抽干线程 = 消息发布者所在的 IO 线程（默认 fanoutThreads == 0，
// 全部内联抽干）。于是同一个目标 loop 会同时存在两条投递路径：
//     M1 由 loop A 的线程抽干 → 对 loop B 走 queueInLoop（排队，尚未执行）
//     M2 由 loop B 的线程抽干 → 对 loop B 走「本线程直投」，抢在 M1 那个批次之前送达
//   ⇒ loop B 上的订阅者先收到 M2、后收到 M1（顺序颠倒，聊天场景直接可见）。
//
// 修复：同一分片的投递一律走 dispatch，使每个目标 loop 只剩一条 FIFO 入口。
// 本用例把这条路径永久焊住 —— 一旦有人改回「本线程直投」，它会立刻失败。
static void testMixedDeliveryOrdering()
{
    std::printf("test: 直投与排队混用时的保序（回归）\n");

    RegDynamic::Options opt;
    opt.maxShards = 1;     // 把变量收敛到「投递路径」这一件事上
    opt.fanoutThreads = 0; // 内联抽干：抽干线程 == 消息发布者所在线程
    RegDynamic reg(opt);

    LoopState loopA;
    loopA.id = 0;
    LoopState loopB;
    loopB.id = 1;

    auto cA = std::make_shared<MockConn>();
    auto cB = std::make_shared<MockConn>();
    reg.subscribe("mix", cA, DynamicLoopHandle{&loopA});
    reg.subscribe("mix", cB, DynamicLoopHandle{&loopB});

    // M1 由 loop A 的线程发布并抽干：对 loop B 只是排队
    tlsCurrentLoop = 0;
    reg.publish("mix", std::string("M1"));
    // M2 由 loop B 的线程发布并抽干：此时 loop B 队列里 M1 的批次尚未执行
    tlsCurrentLoop = 1;
    reg.publish("mix", std::string("M2"));
    tlsCurrentLoop = -1;

    // 两个 loop 各自执行排队的批次（模拟事件循环处理 pending functors）
    auto drainLoop = [](LoopState& l) {
        std::vector<std::function<void()>> take;
        {
            std::lock_guard lock(l.m);
            take.swap(l.pending);
        }
        for (auto& f : take)
        {
            f();
        }
    };
    drainLoop(loopA);
    drainLoop(loopB);

    auto seqOf = [](const std::shared_ptr<MockConn>& c) {
        std::lock_guard lock(c->m);
        return c->received;
    };

    const std::vector<std::string> want{"M1", "M2"};
    CHECK(seqOf(cB) == want,
          "loop B 订阅者顺序 = M1,M2（本线程直投不得抢跑已排队的批次）");
    CHECK(seqOf(cA) == want, "loop A 订阅者顺序 = M1,M2");
}

// ---------------------------------------------------------------------------
// 7. 房间句柄 hint 快路径 + 单非空分片快路径
// ---------------------------------------------------------------------------
static void testRoomHandleHint()
{
    std::printf("test: 房间句柄 hint 快路径 / 单非空分片快路径\n");

    // ── 7a. 句柄命中：带 hint 与不带 hint 必须投到同一个房间 ──────────────
    {
        Reg reg;
        auto c1 = std::make_shared<MockConn>();
        auto c2 = std::make_shared<MockConn>();
        reg.subscribe("hint_room", c1);
        reg.subscribe("hint_room", c2);

        const auto h = reg.acquireRoomHandle("hint_room");
        CHECK(h, "acquireRoomHandle 命中已存在的房间");
        CHECK(!reg.acquireRoomHandle("no_such_room"), "acquireRoomHandle 对不存在的房间返回空句柄");

        CHECK(reg.publish("hint_room", std::string("A"), h), "带 hint 发布成功");
        CHECK(reg.publish("hint_room", std::string("B")), "不带 hint 发布成功");

        const std::vector<std::string> want{"A", "B"};
        CHECK(snapshotOf(c1) == want, "hint 路径投递内容与顺序正确");
        CHECK(snapshotOf(c2) == want, "hint 路径对全部订阅者生效");
    }

    // ── 7b. 句柄失效回退：房间回收后旧句柄不得把消息吞掉 ──────────────────
    {
        Reg reg;
        auto c1 = std::make_shared<MockConn>();
        const auto id1 = reg.subscribe("reborn", c1);
        const auto stale = reg.acquireRoomHandle("reborn");
        reg.unsubscribe("reborn", id1); // 最后一个成员离开 → 房间被回收
        CHECK(!reg.acquireRoomHandle("reborn"), "房间回收后取不到句柄");

        auto c2 = std::make_shared<MockConn>();
        reg.subscribe("reborn", c2); // 同名新房间

        // 关键：传的是【已失效的旧句柄】+ 正确的房间名。
        // 若快路径不判 retired，这条消息会被投进孤儿房间而静默消失。
        CHECK(reg.publish("reborn", std::string("M"), stale), "带失效句柄发布返回成功");
        CHECK(snapshotOf(c2) == std::vector<std::string>{"M"},
              "失效句柄回退查表：消息投给了新房间的订阅者");
        CHECK(snapshotOf(c1).empty(), "失效句柄不会把消息投回旧房间");
    }

    // ── 7c. 单非空分片快路径（dist.size() > 1，但只剩一个分片有人）────────
    // 这是快路径最容易写错的输入：门控说「只有一个非空分片」，而 dist 有多个
    // 槽位 —— 扫描时必须找对那个槽位，否则消息静默丢失。
    {
        Reg::Options opt;
        opt.maxShards = 8;
        opt.subsPerShard = 2;
        opt.fanoutThreads = 2;
        opt.inlineMaxSubs = 0;
        Reg reg(opt);

        auto c1 = std::make_shared<MockConn>();
        auto c2 = std::make_shared<MockConn>();
        auto c3 = std::make_shared<MockConn>();
        reg.subscribe("shardy", c1); // id 1 → 分片 1
        const auto i2 = reg.subscribe("shardy", c2); // id 2 → 分片 0
        reg.subscribe("shardy", c3); // id 3 → 分片 1
        CHECK(reg.activeShards() == 2, "3 个订阅者时房间有 2 个非空分片");

        reg.unsubscribe("shardy", i2); // 分片 0 变空 → 只剩分片 1
        CHECK(reg.activeShards() == 1, "退掉一个后只剩 1 个非空分片（命中快路径）");

        reg.publish("shardy", std::string("S1"));
        reg.publish("shardy", std::string("S2"));

        CHECK(waitUntil(
                  [&] { return snapshotOf(c1).size() == 2 && snapshotOf(c3).size() == 2; }, 2000),
              "单非空分片快路径把消息投给了该分片上的全部订阅者");
        CHECK(snapshotOf(c1) == std::vector<std::string>({"S1", "S2"}),
              "快路径保持分片内 FIFO 顺序");
        CHECK(snapshotOf(c3) == std::vector<std::string>({"S1", "S2"}),
              "快路径对分片内第二个订阅者也生效");
        CHECK(snapshotOf(c2).empty(), "已退订的连接不再收到消息");
    }
}

int main()
{
    testStrictOrdering(0, "线程池并行扇出");
    testStrictOrdering(1000000, "内联扇出");
    testRealParallelism();
    testBackpressure();
    testConcurrentChurn();
    testReshapeOrdering();
    testEdgeCases();
    testLoopGrouping();
    testMixedDeliveryOrdering();
    testRoomHandleHint();

    std::printf("\n%s (failures=%d)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
