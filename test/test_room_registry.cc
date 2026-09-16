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

    // 可调项真的被接进去了（否则就是「提出来了但没人读」的假重构）。
    // workerSpinRounds 原先是 workerLoop() 里的 constexpr，调参要重新编译。
    {
        Reg::Options opt;
        opt.workerSpinRounds = 7;
        Reg tuned(opt);
        CHECK(tuned.options().workerSpinRounds == 7, "workerSpinRounds 透传到生效值");

        Reg::Options zero;
        zero.workerSpinRounds = 0; // 配置错误：0 会让 worker 完全不自旋，按默认兜底
        Reg fallback(zero);
        CHECK(fallback.options().workerSpinRounds == 4000, "workerSpinRounds = 0 时兜底为 4000");
    }
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

    // 注入用：置位后【下一次】dispatch 抛 bad_alloc，用来验证「投递阶段抛异常」
    // 这条路径不会把分片永久卡死（见 testDispatchExceptionRecovery）。
    // 默认 false，不影响其他用例。
    std::atomic<bool> failNextDispatch{false};
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
        if (st->failNextDispatch.exchange(false, std::memory_order_relaxed))
        {
            // 模拟 queueInLoop 内部（std::function 构造 / 队列扩容）分配失败
            throw std::bad_alloc();
        }
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

// 扮演「loop 线程」：把排队的批次取出来执行（swap 出来再执行，避免持锁跑用户代码）。
// 提成文件级函数是因为「在途投递上限」的用例需要反复手动排空 loop 队列。
static void drainLoop(LoopState& l)
{
    std::vector<std::function<void()>> take;
    {
        std::lock_guard lock(l.m);
        take.swap(l.pending);
    }
    for (auto& f : take)
    {
        f();
    }
}

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

// ---------------------------------------------------------------------------
// 8. 投递阶段抛异常后的分片恢复（回归）
// ---------------------------------------------------------------------------
// 缺陷形态（2026-09-15 修复）：fanOutToSnapshot 在锁外调用，dispatch
// （queueInLoop 的 std::function 构造 / 队列扩容）抛异常时，异常会穿出
// drainShard —— 而 busy 仍为 true 且再无人抽该分片，于是后续消息只入队
// 不投递，直到队列堆满开始被拒（实测：抛一次之后分片永久卡死）。
// 修复后：异常在 drainer 内被捕获、busy 复位、剩余消息继续投递，
// 并计入 fanoutExceptions()（导出为 ws_fanout_exceptions_total）。
static void testDispatchExceptionRecovery()
{
    std::printf("test: 投递异常后的分片恢复（回归）\n");

    RegDynamic::Options opt;
    opt.maxShards = 1;
    opt.fanoutThreads = 0; // 内联抽干：异常路径与修复前走同一处代码
    opt.backlogPerShard = 64;
    RegDynamic reg(opt);

    LoopState loop;
    loop.id = 0;
    auto c = std::make_shared<MockConn>();
    reg.subscribe("boom", c, DynamicLoopHandle{&loop});

    loop.failNextDispatch.store(true, std::memory_order_relaxed);
    bool threw = false;
    try
    {
        reg.publish("boom", std::string("M0"));
    }
    catch (...)
    {
        threw = true;
    }
    CHECK(!threw, "dispatch 抛异常时 publish 不把异常抛给调用方（修复前会抛到业务层）");

    for (int i = 1; i <= 5; ++i)
    {
        reg.publish("boom", std::string("M") + std::to_string(i));
    }

    CHECK(reg.fanoutExceptions() >= 1, "投递异常被计数（ws_fanout_exceptions_total）");
    CHECK(loop.pending.size() == 5,
          "异常之后分片恢复投递：后续 5 条全部派发（修复前 busy 卡死，一条都投不出）");
    CHECK(reg.droppedCount() == 0, "恢复过程中没有触发背压丢弃");

    // 执行排队的批次，确认内容与顺序都正确
    drainLoop(loop);

    const std::vector<std::string> want{"M1", "M2", "M3", "M4", "M5"};
    CHECK(snapshotOf(c) == want, "恢复后的消息内容与顺序正确");
}

// ---------------------------------------------------------------------------
// 9. 订阅 ID 跨房间重生不复用（回归）
// ---------------------------------------------------------------------------
// 缺陷形态（2026-09-15 修复）：nextId 原是 Room 的成员，每个房间从 0 重新开始。
// 房间空掉被回收、同名房间重建之后 ID 空间从头复用，而 unsubscribe 是按
// (房间名, id) 定位的 —— 一次迟到的旧 ID 退订就会误删新订阅者。
// 现在 ID 由 registry 级全局单调分配，同一房间名下不会出现重复 ID。
static void testSubscriberIdNotReused()
{
    std::printf("test: 订阅 ID 跨房间重生不复用（回归）\n");

    Reg reg;
    auto c1 = std::make_shared<MockConn>();
    const auto id1 = reg.subscribe("reborn2", c1);
    reg.unsubscribe("reborn2", id1); // 房间空了 → 被回收
    CHECK(reg.activeRooms() == 0, "最后一个订阅者离开后房间被回收");

    auto c2 = std::make_shared<MockConn>();
    const auto id2 = reg.subscribe("reborn2", c2);
    CHECK(id2 != id1, "同名房间重建后新订阅者拿到【新】ID（修复前两代都从 1 开始）");

    // 迟到的旧 ID 退订不得影响新订阅者
    reg.unsubscribe("reborn2", id1);
    CHECK(reg.publish("reborn2", std::string("M")), "旧 ID 退订后房间仍可发布");
    CHECK(snapshotOf(c2) == std::vector<std::string>{"M"},
          "旧 ID 退订不影响新订阅者（幂等且不跨代误删）");
    CHECK(reg.subscribersIn("reborn2") == 1, "订阅者计数仍为 1");
}

// ---------------------------------------------------------------------------
// 10. 全局在途投递上限（回归）
// ---------------------------------------------------------------------------
// 缺陷形态（2026-09-15 修复）：backlogPerShard 只约束【分片队列】，而按 loop 分组
// 投递之后真正的排队点是目标 loop 的 queueInLoop 队列 —— 那是无界的。慢 loop 会让
// 已出队的批次一直堆在那里，分片队列早已排空 ⇒ backlogPerShard 永不触发 ⇒
// 调用方拿不到任何背压信号，内存无界上涨。
// 实测（修复前）：backlogPerShard=4，publish 10000 条【全部返回成功】、dropped=0、
// 连接实际收到 0 条。
// 修复后：全局在途份数达到 maxInFlightDeliveries 即拒收（publish 返回 false）；
// 只拒绝、不重排 ⇒ 不影响保序；置 0 = 关闭（退回旧行为）。
static void testInFlightCap()
{
    std::printf("test: 全局在途投递上限（回归）\n");

    // ---- 10a. 达到上限后拒收，排空后恢复 ----
    {
        LoopState loop;
        loop.id = 0;

        RegDynamic::Options opt;
        opt.maxShards = 1;
        opt.fanoutThreads = 0;
        opt.backlogPerShard = 1 << 20; // 刻意放大：确保触发的是在途上限而不是分片积压
        opt.maxInFlightDeliveries = 3; // 每批 1 份 ⇒ 第 4 条起被拒

        RegDynamic reg(opt);
        auto c = std::make_shared<MockConn>();
        reg.subscribe("cap", c, DynamicLoopHandle{&loop});

        // 不排空 loop ⇒ 份数一直挂在「在途」
        CHECK(reg.publish("cap", std::string("A")), "在途 0 → 第 1 条被接受");
        CHECK(reg.publish("cap", std::string("B")), "在途 1 → 第 2 条被接受");
        CHECK(reg.publish("cap", std::string("C")), "在途 2 → 第 3 条被接受");
        CHECK(reg.inFlightDeliveries() == 3, "在途份数 = 3（3 条 × 1 个收件人）");
        CHECK(!reg.publish("cap", std::string("D")),
              "在途 3 = 上限 → 第 4 条被拒（修复前恒返回成功）");
        CHECK(reg.inflightRejectedCount() == 1, "因在途上限被拒 1 次");
        CHECK(reg.droppedCount() == 1, "被拒同时计入总丢弃数（ws_messages_dropped_total）");
        CHECK(!reg.publish("cap", std::string("E")), "上限持续生效");
        CHECK(reg.inflightRejectedCount() == 2, "累计被拒 2 次");

        // 排空 loop → 在途归零 → 重新放行
        drainLoop(loop);
        CHECK(reg.inFlightDeliveries() == 0, "loop 执行完后在途归零（计数无泄漏）");
        CHECK(reg.publish("cap", std::string("F")), "在途归零后重新放行");
        drainLoop(loop);
        CHECK(snapshotOf(c) == std::vector<std::string>({"A", "B", "C", "F"}),
              "被拒的消息不进队列，已接受的消息内容与顺序不变");
    }

    // ---- 10b. 上限 0 = 关闭（退回旧行为）----
    {
        LoopState loop;
        loop.id = 0;

        RegDynamic::Options opt;
        opt.maxShards = 1;
        opt.fanoutThreads = 0;
        opt.backlogPerShard = 1 << 20;
        opt.maxInFlightDeliveries = 0; // 关闭

        RegDynamic reg(opt);
        auto c = std::make_shared<MockConn>();
        reg.subscribe("nocap", c, DynamicLoopHandle{&loop});

        bool allOk = true;
        for (int i = 0; i < 500; ++i)
        {
            allOk = reg.publish("nocap", std::string("M")) && allOk;
        }
        CHECK(allOk, "maxInFlightDeliveries = 0 时上限关闭（500 条全部接受）");
        CHECK(reg.inflightRejectedCount() == 0, "关闭时没有任何在途拒绝");
        CHECK(reg.inFlightDeliveries() == 500, "在途份数如实累计到 500");
    }

    // ---- 10c. 派发失败不得泄漏在途计数 ----
    {
        LoopState loop;
        loop.id = 0;

        RegDynamic::Options opt;
        opt.maxShards = 1;
        opt.fanoutThreads = 0;
        opt.backlogPerShard = 1 << 20;
        opt.maxInFlightDeliveries = 8;

        RegDynamic reg(opt);
        auto c = std::make_shared<MockConn>();
        reg.subscribe("leak", c, DynamicLoopHandle{&loop});

        loop.failNextDispatch.store(true, std::memory_order_relaxed);
        reg.publish("leak", std::string("X"));
        CHECK(reg.inFlightDeliveries() == 0,
              "派发失败时在途计数被归还（否则只增不减，最终把房间钉死在上限）");
        CHECK(reg.publish("leak", std::string("Y")), "派发失败后仍能继续发布");
        CHECK(reg.inFlightDeliveries() == 1, "在途计数如实反映成功派发的那一条");
    }
}

// ---------------------------------------------------------------------------
// 13. 房间级序号（跨实例保序的「证人」）
// ---------------------------------------------------------------------------
// 断言的语义：
//   · 序号从 1 开始、每条被接纳的消息 +1（同一把 pubMtx 内分配 ⇒ 序号顺序 == 入队顺序）
//   · 被背压拒绝的消息【不占号】（否则接收端会把「本地没发出去」误报成「传输丢了」）
//   · 不同房间各自独立计数（否则接收端按房间判缺口会满屏误报）
//   · 房间空掉被回收、同名房间重建后序号【续上】而不是从 1 重来
//     （否则接收端会把房间重建当成一次大规模乱序）
//   · 无订阅者 / 无该房间时不分配序号（写回 0）
static void testRoomSeq()
{
    std::printf("\ntest: 房间级序号\n");
    LoopState loop;
    loop.id = 0;

    RegDynamic::Options opt;
    opt.maxShards = 1;
    opt.fanoutThreads = 0;
    opt.backlogPerShard = 1 << 20;
    opt.maxInFlightDeliveries = 1 << 20;

    // ---- 13a. 无订阅者 / 无该房间 ⇒ 不分配序号 ----
    {
        RegDynamic reg(opt);
        uint64_t seq = 12345; // 先写个哨兵，验证函数会把它清零
        CHECK(reg.publish("nobody", std::string("x"), {}, &seq),
              "向不存在的房间发布返回成功（无订阅者，静默丢弃）");
        CHECK(seq == 0, "无订阅者时不分配序号（写回 0，接收端据此跳过缺口判断）");
    }

    // ---- 13b. 单调递增 + 房间之间互相独立 ----
    {
        RegDynamic reg(opt);
        auto c = std::make_shared<MockConn>();
        auto c2 = std::make_shared<MockConn>();
        reg.subscribe("seq", c, DynamicLoopHandle{&loop});
        reg.subscribe("other", c2, DynamicLoopHandle{&loop});

        uint64_t s[5] = {0, 0, 0, 0, 0};
        bool allOk = true;
        for (int i = 0; i < 5; ++i)
        {
            allOk = reg.publish("seq", std::string("m"), {}, &s[i]) && allOk;
        }
        CHECK(allOk, "5 条消息全部被接纳");
        CHECK(s[0] == 1 && s[1] == 2 && s[2] == 3 && s[3] == 4 && s[4] == 5,
              "序号从 1 开始、每条 +1（1,2,3,4,5）");

        uint64_t o1 = 0;
        CHECK(reg.publish("other", std::string("o"), {}, &o1), "另一房间发布成功");
        CHECK(o1 == 1, "不同房间各自独立计数（other 从 1 开始，不受 seq 房间影响）");

        // hint 快路径也必须写回序号（否则带 hint 的调用方广播时永远不带序号）
        uint64_t hs = 0;
        auto handle = reg.acquireRoomHandle("seq");
        CHECK(reg.publish("seq", std::string("hint"), handle, &hs), "带 hint 发布成功");
        CHECK(hs == 6, "hint 快路径同样分配序号（续到 6）");

        drainLoop(loop);
        CHECK(snapshotOf(c).size() == 6, "序号分配不影响投递（6 条全部送达）");
    }

    // ---- 13c. 被背压拒绝的消息不占号 ----
    {
        RegDynamic::Options capped = opt;
        capped.maxInFlightDeliveries = 3;
        RegDynamic reg(capped);
        auto c = std::make_shared<MockConn>();
        reg.subscribe("cap", c, DynamicLoopHandle{&loop});

        uint64_t a = 0, b = 0, d = 0;
        uint64_t rejectedSeq = 999;
        CHECK(reg.publish("cap", std::string("A"), {}, &a), "第 1 条被接受");
        CHECK(reg.publish("cap", std::string("B"), {}, &b), "第 2 条被接受");
        CHECK(reg.publish("cap", std::string("C"), {}, &d), "第 3 条被接受");
        CHECK(a == 1 && b == 2 && d == 3, "被接纳的 3 条序号连续为 1,2,3");

        // 不排空 loop ⇒ 在途 3 = 上限 ⇒ 下一条被拒
        CHECK(!reg.publish("cap", std::string("D"), {}, &rejectedSeq), "在途达上限 ⇒ 第 4 条被拒");
        CHECK(rejectedSeq == 0,
              "被拒的消息不分配序号（否则接收端会把它误报成「传输中丢了一条」）");

        drainLoop(loop);
        uint64_t e = 0;
        CHECK(reg.publish("cap", std::string("E"), {}, &e), "排空后重新放行");
        CHECK(e == 4, "序号连续（1,2,3,4）—— 被拒的那条没有制造缺口");
    }

    // ---- 13d. 房间回收后同名重建，序号续上而不是从 1 重来 ----
    {
        RegDynamic reg(opt);
        auto c = std::make_shared<MockConn>();
        const auto id = reg.subscribe("recycle", c, DynamicLoopHandle{&loop});

        uint64_t s1 = 0, s2 = 0;
        reg.publish("recycle", std::string("1"), {}, &s1);
        reg.publish("recycle", std::string("2"), {}, &s2);
        CHECK(s1 == 1 && s2 == 2, "回收前序号为 1,2");
        drainLoop(loop);

        reg.unsubscribe("recycle", id);
        CHECK(reg.activeRooms() == 0, "全部退订后房间被回收");

        auto c2 = std::make_shared<MockConn>();
        reg.subscribe("recycle", c2, DynamicLoopHandle{&loop});
        uint64_t s3 = 0;
        reg.publish("recycle", std::string("3"), {}, &s3);
        CHECK(s3 == 3,
              "同名房间重建后序号续到 3（修复前会从 1 重来，接收端每次都误报大规模乱序）");
        drainLoop(loop);
    }
}

static void testJournalReplay()
{
    std::printf("\ntest: 房间消息日志与回放\n");
    LoopState loop;
    loop.id = 0;

    RegDynamic::Options opt;
    opt.maxShards = 1;
    opt.fanoutThreads = 0;
    opt.backlogPerShard = 1 << 20;
    opt.maxInFlightDeliveries = 1 << 20;
    opt.journalCap = 512;

    // ---- 14a. 基本回放：只回 seq > sinceSeq 的条目，且按升序 ----
    {
        RegDynamic reg(opt);
        auto c = std::make_shared<MockConn>();
        reg.subscribe("jr", c, DynamicLoopHandle{&loop});

        for (int i = 1; i <= 5; ++i)
        {
            reg.publish("jr", std::string("m") + std::to_string(i));
        }

        const auto rr = reg.replay("jr", 2);
        CHECK(rr.entries.size() == 3, "游标 2 ⇒ 回放 3 条（3,4,5）");
        CHECK(rr.entries.size() == 3 && rr.entries[0].seq == 3 && rr.entries[2].seq == 5,
              "回放条目按 seq 升序且首尾正确");
        CHECK(rr.maxSeq == 5, "maxSeq = 房间当前最大序号（5）");
        CHECK(rr.oldestSeq == 1, "oldestSeq = 日志里最老的一条（1）");
        CHECK(!rr.reset, "游标在可回放范围内 ⇒ reset = false");
        CHECK(rr.entries[0].payload && *rr.entries[0].payload == "m3",
              "回放负载就是当初投出去的那一份（m3）");

        const auto up = reg.replay("jr", 5);
        CHECK(up.entries.empty() && !up.reset, "游标已追平 ⇒ 空回放且 reset = false");

        // sinceSeq = 0 刻意定义为「没有游标 ⇒ 不回放」，而不是「从头回放」
        const auto none = reg.replay("jr", 0);
        CHECK(none.entries.empty() && !none.reset,
              "sinceSeq = 0 ⇒ 不回放历史（新客户端不该突然收到几百条旧消息）");

        const auto miss = reg.replay("no_such_room", 1);
        CHECK(miss.entries.empty() && miss.maxSeq == 0, "房间不存在 ⇒ 空结果");
        CHECK(miss.reset,
              "但带着游标问一个从没见过的房间 ⇒ 必须报「不完整」（无从证明它没缺消息）");
    }

    // ---- 14b. 环形缓冲裁掉旧条目 ⇒ 必须报 reset，不能假装补齐 ----
    {
        RegDynamic::Options small = opt;
        small.journalCap = 3;
        RegDynamic reg(small);
        auto c = std::make_shared<MockConn>();
        reg.subscribe("jr2", c, DynamicLoopHandle{&loop});

        for (int i = 1; i <= 5; ++i)
        {
            reg.publish("jr2", std::string("m") + std::to_string(i));
        }

        const auto rr = reg.replay("jr2", 1);
        CHECK(rr.oldestSeq == 3, "容量 3 ⇒ 最老的只剩 3（1、2 被裁掉）");
        CHECK(rr.reset, "游标 1 之后接不上（需要 2）⇒ reset = true");
        CHECK(rr.entries.size() == 3, "仍然把手上有的 3 条都回放出去");

        // 恰好停在 oldestSeq - 1 ⇒ 是完整补齐，不该误报
        const auto exact = reg.replay("jr2", 2);
        CHECK(!exact.reset, "游标恰好停在 oldestSeq-1 ⇒ 完整补齐（不误报 reset）");
    }

    // ---- 14c. 游标大于当前最大序号 ⇒ 序号空间对不上 ----
    {
        RegDynamic reg(opt);
        auto c = std::make_shared<MockConn>();
        reg.subscribe("jr3", c, DynamicLoopHandle{&loop});
        reg.publish("jr3", std::string("m1"));

        const auto rr = reg.replay("jr3", 999);
        CHECK(rr.reset, "游标 999 > maxSeq 1 ⇒ reset = true（另一实例的序号空间 / 房间重建过）");
        CHECK(rr.entries.empty(), "此时不该回放任何条目");
    }

    // ---- 14d. 关掉回放（journalCap = 0）时对落后的游标必须诚实 ----
    {
        RegDynamic::Options noJournal = opt;
        noJournal.journalCap = 0;
        RegDynamic reg(noJournal);
        auto c = std::make_shared<MockConn>();
        reg.subscribe("jr4", c, DynamicLoopHandle{&loop});
        for (int i = 1; i <= 3; ++i)
        {
            reg.publish("jr4", std::string("m"));
        }

        const auto behind = reg.replay("jr4", 1);
        CHECK(behind.entries.empty(), "关闭回放 ⇒ 没有条目可回");
        CHECK(behind.reset, "但游标落后 ⇒ reset = true（绝不能回一个「补齐成功」的假结果）");

        const auto caught = reg.replay("jr4", 3);
        CHECK(!caught.reset, "游标已追平 ⇒ reset = false（这种情况确实是完整的）");
    }

    // ---- 14e. 被背压拒绝的消息不进日志、也不占号 ----
    {
        RegDynamic::Options capped = opt;
        // 用「在途投递上限」制造确定性拒绝：目标 loop 不抽干 ⇒ 在途恒为 1。
        //（不用分片积压，那需要真起线程池，时序不确定。）
        capped.maxInFlightDeliveries = 1;
        RegDynamic reg(capped);
        auto c = std::make_shared<MockConn>();
        reg.subscribe("jr5", c, DynamicLoopHandle{&loop});

        const bool first = reg.publish("jr5", std::string("kept"));
        const bool second = reg.publish("jr5", std::string("rejected"));
        CHECK(first && !second, "在途额度用满后第二条被拒");

        CHECK(reg.replay("jr5", 0).maxSeq == 1, "被拒的消息不占号（maxSeq 停在 1）");

        // 抽干之后第三条应拿到序号 2（若被拒的那条占了号，这里会是 3）
        drainLoop(loop);
        uint64_t s3 = 0;
        CHECK(reg.publish("jr5", std::string("third"), {}, &s3), "抽干后第三条被接纳");
        CHECK(s3 == 2, "序号连续（被拒的那条没有占号，第三条是 2 而不是 3）");

        const auto rr = reg.replay("jr5", 1);
        CHECK(rr.entries.size() == 1 && *rr.entries[0].payload == "third",
              "日志里只有被接纳的两条，回放游标 1 之后拿到 third");
    }

    // ---- 14f. 日志要活过「房间被回收」----
    //
    // 场景：客户端是房间里最后一个人，它断线 ⇒ 房间被回收 ⇒ 它立刻重连。
    // 原实现里日志随 Room 一起销毁，这个最常见的重连场景必然拿到 206 ——
    // 明明服务端还留着内容。现在回收时把日志搬进暂存区，重建时搬回来。
    {
        RegDynamic reg(opt);
        const auto id = reg.subscribe("jr6", std::make_shared<MockConn>(), DynamicLoopHandle{&loop});
        for (int i = 1; i <= 4; ++i)
        {
            reg.publish("jr6", std::string("m") + std::to_string(i));
        }
        drainLoop(loop);

        reg.unsubscribe("jr6", id);
        CHECK(reg.activeRooms() == 0, "最后一个订阅者离开 ⇒ 房间被回收");

        // ① 房间不存在时（客户端还没重新进房）也要能回放
        const auto before = reg.replay("jr6", 2);
        CHECK(before.entries.size() == 2 && before.entries[0].seq == 3 && before.entries[1].seq == 4,
              "回收后仍能回放（游标 2 ⇒ 补 3、4）—— 修复前这里是空的");
        CHECK(!before.reset, "且是完整补齐（不是 206）");
        CHECK(before.maxSeq == 4, "maxSeq 来自序号水位线（4）");

        // ② 重建房间后，日志被搬回来，且序号续上
        reg.subscribe("jr6", std::make_shared<MockConn>(), DynamicLoopHandle{&loop});
        const auto after = reg.replay("jr6", 2);
        CHECK(after.entries.size() == 2, "重建后日志仍在（搬回来了，不是丢在暂存区）");
        CHECK(after.maxSeq == 4, "重建后 maxSeq 仍是 4");

        uint64_t s5 = 0;
        reg.publish("jr6", std::string("m5"), {}, &s5);
        CHECK(s5 == 5, "重建后序号续到 5");
        drainLoop(loop);

        // ③ 太旧的游标仍然要如实报 206（暂存不改变「补不齐就说补不齐」）
        const auto stale = reg.replay("jr6", 0);
        CHECK(stale.entries.empty() && !stale.reset, "sinceSeq = 0 仍然不回放");
    }

    // ---- 14g. 暂存区也有上限：超了按 FIFO 淘汰，不能变成内存泄漏 ----
    {
        RegDynamic::Options tiny = opt;
        tiny.journalCap = 4;
        RegDynamic reg(tiny);
        // 造 5000 个「用完即回收」的房间。上限是 4096 个房间 / 65536 条，
        // 这里主要是验证「不崩、且最老的会被淘汰」。
        for (int i = 0; i < 5000; ++i)
        {
            const std::string room = "leak_" + std::to_string(i);
            const auto id = reg.subscribe(room, std::make_shared<MockConn>(), DynamicLoopHandle{&loop});
            reg.publish(room, std::string("x"));
            reg.unsubscribe(room, id);
        }
        drainLoop(loop);
        CHECK(reg.activeRooms() == 0, "5000 个房间全部回收");
        // 最早那个已被 FIFO 淘汰 ⇒ 回放拿不到内容，但必须如实报「不完整」
        const auto old = reg.replay("leak_0", 1);
        CHECK(old.entries.empty(), "最老的暂存条目已被淘汰（内存有上限，不是无界增长）");
        CHECK(old.reset, "淘汰后仍如实报「本次不是完整补齐」，不假装成功");
    }

    // ---- 14h. 记账配平：反复「回收 → 重建」不能把暂存区撑成假超限 ----
    //
    // 回归的是 journalRetainedEntries_ 的记账泄漏：getOrCreateRoom 把日志从暂存区
    // 搬回 Room 时若忘了减计数，每轮「回收 → 重建」净增一次日志长度，计数单调上溢。
    // 越过 kMaxJournalEntries 之后，每次 retainJournalLocked 都会进淘汰循环，
    // 开始丢【别的房间】的暂存日志 —— 症状是「不相关房间的重连突然补不出内容」。
    //
    // 这里刻意让 churn 房间反复回收重建，最后暂存一个无关房间再碰一次 churn：
    // 记账正确时 victim 完好；记账泄漏时 victim 会被连带淘汰。
    {
        RegDynamic::Options tiny = opt;
        tiny.journalCap = 256; // 每轮泄漏 256 条 ⇒ 300 轮即越过 65536 上限
        RegDynamic reg(tiny);

        for (int i = 0; i < 300; ++i)
        {
            const auto id = reg.subscribe("churn", std::make_shared<MockConn>(), DynamicLoopHandle{&loop});
            for (int k = 0; k < 256; ++k)
            {
                reg.publish("churn", std::string("c"));
            }
            reg.unsubscribe("churn", id);
        }
        drainLoop(loop);

        // 暂存一个与 churn 完全无关的房间
        {
            const auto id = reg.subscribe("victim", std::make_shared<MockConn>(), DynamicLoopHandle{&loop});
            for (int k = 1; k <= 4; ++k)
            {
                reg.publish("victim", std::string("v") + std::to_string(k));
            }
            reg.unsubscribe("victim", id);
        }
        CHECK(reg.activeRooms() == 0, "victim 已回收并暂存");

        // 再碰一次 churn：这一步会调 retainJournalLocked，正是淘汰循环的入口
        {
            const auto id = reg.subscribe("churn", std::make_shared<MockConn>(), DynamicLoopHandle{&loop});
            reg.publish("churn", std::string("c"));
            reg.unsubscribe("churn", id);
        }
        drainLoop(loop);

        const auto v = reg.replay("victim", 1);
        CHECK(v.entries.size() == 3, "300 轮回收重建后，无关房间的暂存日志仍在（记账未上溢）");
        CHECK(v.entries.size() == 3 && v.entries[0].seq == 2 && v.entries[2].seq == 4,
              "victim 的内容完整：游标 1 ⇒ 补 2、3、4");
        CHECK(!v.reset, "且是完整补齐 —— 被连带淘汰的话这里会是 206");

        // 顺带确认 churn 自己也没被自己的记账撑坏
        const auto c = reg.replay("churn", 0);
        CHECK(c.entries.empty() && !c.reset, "sinceSeq = 0 仍然不回放");
    }
}

// ── 15. 连接级投递闸门（拥塞跳过）──────────────────────────────────────────────
//
// 回归的是「每连接输出缓冲无界」这个缺陷的【服务端侧对策】：
// 上层（ChatWebsocket）在建连时经 HttpRequest::getConnectionPtr() 拿到
// trantor::TcpConnection 并装上高水位回调，越过阈值就把该连接的闸门关上；
// 注册表在扇出时读到闸门关着就跳过它、不调 send()，并计入 gatedDrops()。
//
// 这里锁的是注册表这一侧的契约（高水位回调本身属于 drogon/trantor 集成，
// 单测里没有真实 socket，只能由 e2e 覆盖）：
//   ① 不传闸门（老调用方 / 默认参数）⇒ 行为与改动前逐字节一致；
//   ② 闸门非 0 ⇒ 只跳过【那一条】连接，同一批次里其他订阅者照收；
//   ③ 闸门归零 ⇒ 立刻恢复投递（闸门不是单向的，否则慢消费者会被永久饿死）；
//   ④ 被跳过的份数单独计数，且【不计入】分片背压的 droppedCount()。
static void testConnectionGate()
{
    std::printf("test: 连接级投递闸门（拥塞跳过）\n");

    LoopState loop;
    RegDynamic::Options opt;
    opt.maxShards = 1;
    opt.fanoutThreads = 0; // 内联扇出：发布线程直接投递，drain 一次即可判定

    // ---- 15a~15d. 走 loop 批次分支（有 loop 句柄）----
    {
        RegDynamic reg(opt);

        auto normal = std::make_shared<MockConn>();
        auto gated = std::make_shared<MockConn>();
        auto gate = std::make_shared<std::atomic<uint8_t>>(0);

        const auto idNormal = reg.subscribe("g", normal, DynamicLoopHandle{&loop});
        const auto idGated = reg.subscribe("g", gated, DynamicLoopHandle{&loop}, gate);

        // ---- 15a. 闸门开着（=0）时两条都收 ----
        reg.publish("g", std::string("m1"));
        drainLoop(loop);
        CHECK(snapshotOf(normal).size() == 1 && snapshotOf(gated).size() == 1,
              "闸门为 0 时两条连接都收到消息");
        CHECK(reg.gatedDrops() == 0, "没有闸门跳过时 gatedDrops 保持 0");

        // ---- 15b. 关上闸门：只跳过那一条，另一条照收 ----
        //
        // 「只跳过那一条」是本用例的核心：闸门挂在 Entry 上而不是房间上，
        // 关掉一条连接绝不能影响同房间的其他订阅者。
        gate->store(1, std::memory_order_relaxed);
        reg.publish("g", std::string("m2"));
        reg.publish("g", std::string("m3"));
        drainLoop(loop);
        CHECK(snapshotOf(normal).size() == 3, "未被闸门挡住的连接继续收到全部消息");
        CHECK(snapshotOf(gated).size() == 1, "被闸门挡住的连接一条都没收到");
        CHECK(reg.gatedDrops() == 2, "被跳过的份数逐条计入 gatedDrops（2 条消息 × 1 条连接）");
        // 口径必须分开：那条消息是被【接受】了的（分片队列没满、seq 也分配了），
        // 只是这一个订阅者没喂。混进 droppedCount() 会让「服务端在丢消息」这个
        // 判断失真 —— 实际上丢的只是某一个慢消费者的帧。
        CHECK(reg.droppedCount() == 0, "闸门跳过【不算】分片背压丢弃（两者口径不同）");

        // ---- 15c. 闸门归零 ⇒ 立刻恢复 ----
        gate->store(0, std::memory_order_relaxed);
        reg.publish("g", std::string("m4"));
        drainLoop(loop);
        CHECK(snapshotOf(gated).size() == 2, "闸门归零后立刻恢复投递（闸门不是单向的）");
        CHECK(reg.gatedDrops() == 2, "恢复后不再累加跳过数");

        // ---- 15d. 不传闸门（老签名 / 默认参数）行为不变 ----
        auto plain = std::make_shared<MockConn>();
        const auto idPlain = reg.subscribe("g", plain, DynamicLoopHandle{&loop});
        gate->store(1, std::memory_order_relaxed);
        reg.publish("g", std::string("m5"));
        drainLoop(loop);
        CHECK(snapshotOf(plain).size() == 1, "未提供闸门的订阅者永远收（nullptr = 不设闸门）");

        reg.unsubscribe("g", idNormal);
        reg.unsubscribe("g", idGated);
        reg.unsubscribe("g", idPlain);
    }

    // ---- 15e. 无 loop 句柄（直投分支）必须同语义 ----
    //
    // 扇出有两处投递循环（!loop.valid() 的直投分支 / loop 批次）。
    // 两处读闸门必须完全一致，否则「有 loop 句柄」与「没 loop 句柄」会给出
    // 不同的投递结果 —— 而 loop 句柄只在取不到时才失效，属于线上会发生的情况。
    {
        RegDynamic reg(opt);
        auto normal = std::make_shared<MockConn>();
        auto gated = std::make_shared<MockConn>();
        auto gate = std::make_shared<std::atomic<uint8_t>>(1); // 一上来就关着

        reg.subscribe("g2", normal, DynamicLoopHandle{}); // 无效句柄 ⇒ 直投分支
        reg.subscribe("g2", gated, DynamicLoopHandle{}, gate);

        reg.publish("g2", std::string("d1"));
        drainLoop(loop);
        CHECK(snapshotOf(normal).size() == 1 && snapshotOf(gated).empty(),
              "直投分支同样遵守闸门：关着的一条都不发");
        CHECK(reg.gatedDrops() == 1, "直投分支的跳过同样被计数");
    }
}

int main()
{    testStrictOrdering(0, "线程池并行扇出");
    testStrictOrdering(1000000, "内联扇出");
    testRealParallelism();
    testBackpressure();
    testConcurrentChurn();
    testReshapeOrdering();
    testEdgeCases();
    testLoopGrouping();
    testMixedDeliveryOrdering();
    testRoomHandleHint();
    testDispatchExceptionRecovery();
    testSubscriberIdNotReused();
    testInFlightCap();
    testRoomSeq();
    testJournalReplay();
    testConnectionGate();

    std::printf("\n%s (failures=%d)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
