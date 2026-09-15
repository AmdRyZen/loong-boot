//
// Created by 神圣•凯莎 on 26-9-11.
//
// RoomRegistry —— 房间级「按订阅者分片 + 多核并行扇出」的发布订阅注册表
// ===========================================================================
// 为什么需要它（旧实现的真实瓶颈）：
//   drogon::PubSubService<Topic>::publish() 在 shared_lock 下【单线程】遍历全部订阅者，
//   而 WebSocketConnectionImpl::sendWsData() 对每个订阅者都要：
//       bytesFormatted.resize(len + 10)   // 一次堆分配
//       bytesFormatted.append(msg, len)   // 一次全量 memcpy
//   于是单房间一次扇出的 CPU 成本 = O(订阅者数 × 消息长度)，且全部压死在一个核上。
//   房间越大 → 单核越忙 → 极限吞吐越低。这就是「去掉 TBB 中转后极限性能下降」的根因。
//
// 设计（严格保序 + 多核并行，二者不冲突）：
//   1. 每个房间有固定 K 个分片槽位（K = maxShards），订阅者按 id % K 恒定归属某个分片，
//      【永不迁移】。因为不存在「分片数变化 → 成员重分布」这一步，也就没有
//      「旧分片残留消息晚于新分片消息投递」的乱序窗口。空槽位不建对象、发布时跳过，
//      所以小房间的开销与成员数成正比，不会为用不到的分片买单。
//   2. 每个分片有独立的 FIFO 队列，队列内部严格串行 → 同分片订阅者天然有序。
//      并行的永远是「不同订阅者」，不是「同一条消息的不同副本」。
//   3. 房间级发布锁只负责「按到达顺序把 payload 压进各分片队列」这一小段临界区，
//      真正的扇出（send）在锁外由扇出线程池并行完成。
//      —— 顺序建立在【入队顺序】上，而不是【扇出顺序】上，所以并行不影响保序。
//   4. 分片订阅者列表用 copy-on-write 快照：扇出线程每条消息只做一次 shared_ptr 拷贝，
//      不产生 O(N) 的引用计数风暴；增删订阅者只重建受影响分片（O(N/K)）。
//   5. 分片队列有上限，超限显式丢弃并返回 false（由调用方计数 + 告知客户端），不静默丢。
//   6. 小房间（订阅者 ≤ inlineMaxSubs）直接内联扇出，与旧实现等价，不惊动线程池。
//
// 与旧实现的另一个差异：去掉了 std::function 回调中转，分片直接持有连接指针。
//
// ---------------------------------------------------------------------------
// 投递侧的第二个瓶颈（A1，2026-09-12）—— 跨线程唤醒
// ---------------------------------------------------------------------------
// 去掉 std::function 中转之后，扇出循环本身已经不再是瓶颈，但吞吐仍卡在
// ~40 万次投递/s。真正的原因在 drogon/trantor 的 send 路径：
//   TcpConnectionImpl::send() 若发现当前不在该连接的 loop 线程上，就
//   loop->queueInLoop(lambda) —— 一次 std::function 构造 + 入队 + 一次唤醒
//   系统调用（macOS 上是对 socketpair 的 write，再让 kevent 从等待中返回），
//   实测合计 ≈ 2.5µs/份。
// 而本工程 IO 线程数是 hardware_concurrency()*2（main.cc 在 loadConfigFile
// 之后又调了一次 setThreadNum，会覆盖 config.json 的 number_of_threads），
// 订阅者被分散到各个 loop，于是【一次扇出里绝大多数投递都是跨线程的】。
//
// 解法就是本文件的 LoopH 分组：快照按「连接所属 loop」预分组，每个目标 loop
// 每条消息只唤醒一次、携带整批连接，实际 send 在目标 loop 线程内完成 →
// 唤醒次数由 O(订阅者数) 降为 O(IO 线程数)。
//
// 这也解释了为什么「多分片并行扇出」从来没用：它并行的是扇出循环，而瓶颈
// 在扇出循环之外的唤醒路径上，加线程只多付记账与上下文切换成本。
// ---------------------------------------------------------------------------
//
// 连接类型做成模板参数，唯一要求是「可 bool 判空 + 有 send(std::string_view)」。
// LoopH 是第二个模板参数（事件循环句柄），默认 NullLoopHandle = 不分组的直投，
// 单测因此不需要拉起 drogon 运行时。
// 生产环境实例化为 RoomRegistry（= RoomRegistryT<drogon::WebSocketConnectionPtr,
// TrantorLoopHandle>），单测用轻量 mock 连接 + mock loop。
// ===========================================================================
//
#ifndef LOONG_BOOT_ROOM_REGISTRY_H
#define LOONG_BOOT_ROOM_REGISTRY_H

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <drogon/WebSocketConnection.h>
#include <trantor/net/EventLoop.h>
#include "parallel_hashmap/phmap.h"

// ===========================================================================
// Loop 句柄：扇出分组的依据（A1 优化）
// ===========================================================================
// 为什么必须分组：drogon 的投递路径 TcpConnectionImpl::send(std::string&&) 在
// 【非本 loop 线程】时会走 loop->queueInLoop(lambda)，代价 = 一次 std::function
// 构造 + 入队 + 一次唤醒系统调用（macOS 上是对 socketpair 的 write + 让 kevent
// 从等待中返回），实测合计 ≈ 2.5µs/份。
//
// 而本工程的 IO 线程数是 hardware_concurrency()*2（见 main.cc —— 注意它在
// loadConfigFile 之后又调了一次 setThreadNum，会覆盖 config.json 里的
// number_of_threads），订阅者被内核轮询分散到各个 loop。于是【一次房间扇出里
// 绝大多数投递都是跨线程的】，这才是单房间扇出卡在 ~40 万次投递/s 的真正原因
// ——不是扇出循环本身慢（分片并行因此毫无收益）。
//
// 分组后：每个目标 loop 每条消息只 queueInLoop 一次、携带该 loop 上的整批连接，
// 实际投递在各自 loop 的线程内完成 → 走 isInLoopThread() 快路径，
// 唤醒次数从 O(订阅者数) 降到 O(IO 线程数)。
//
// 保序不受影响：分组在【快照重建】时一次性算好（连接所属 loop 永不改变），
// 投递仍按分片 FIFO 逐个 payload 派发；同一 loop 的 queueInLoop 本身是 FIFO，
// 所以同一订阅者看到的消息顺序与单线程直投完全一致。
// ===========================================================================

// 无 loop（单测、或调用方没提供 loop 时的回退）：永远视作「就在本线程」，直接投递。
struct NullLoopHandle
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
    bool operator==(const NullLoopHandle&) const noexcept
    {
        return true;
    }
};

// 生产用：包装 trantor::EventLoop*。
// dispatch 刻意做成模板成员 —— 只有真正被实例化时才引用 EventLoop::queueInLoop，
// 因此不链接 drogon/trantor 的单测不会产生未定义符号。
struct TrantorLoopHandle
{
    trantor::EventLoop* loop = nullptr;

    bool valid() const noexcept
    {
        return loop != nullptr;
    }
    // loop 为空时按「本线程」处理，即回退到直接投递（安全兜底）
    bool isCurrentThread() const noexcept
    {
        return loop == nullptr || loop->isInLoopThread();
    }
    template <typename F>
    void dispatch(F&& f) const
    {
        loop->queueInLoop(std::forward<F>(f));
    }
    bool operator==(const TrantorLoopHandle& o) const noexcept
    {
        return loop == o.loop;
    }
};

template <typename ConnT, typename LoopH = NullLoopHandle>
class RoomRegistryT
{
  private:
    // 房间实现（定义在下面的 private 区）。先声明后定义，是为了让下面的
    // RoomHandle 能作为公开类型暴露出去，而 Room 的布局仍然不对外可见。
    //
    // 注意：嵌套类的「声明」与「定义」必须处于同一访问级别，
    // 否则 GCC 报 "redeclared with different access"（已实测）。
    struct Room;

  public:
    using SubscriberID = uint64_t;
    using ConnPtr = ConnT;

    // 房间句柄（不透明）：订阅成功后取一次并缓存起来，之后发布时把它传回来，
    // 就能跳过「每次发布都拿全局 roomsMtx_ 共享锁 + 按房间名哈希查表」这一步。
    //
    // 外部只会拷贝/析构这个 shared_ptr，不需要（也看不到）Room 的完整类型 ——
    // shared_ptr 的 deleter 在 RoomRegistry 内部构造时就已类型擦除。
    using RoomHandle = std::shared_ptr<Room>;

    struct Options
    {
        // 单房间最大分片槽位数 = 单房间最大并行扇出度。
        // 默认 1：单分片 + 内联扇出，与旧实现语义等价，但少了 std::function 中转与双层队列。
        //
        // 实测：单房间扇出吞吐与旧实现持平（确定性基准 200 订阅者：约 35~42 万次投递/s）。
        // 瓶颈不在扇出循环本身，而在 drogon 每份投递的 send 路径（跨线程 queueInLoop
        // + 每份一次堆分配），所以【分片并行无法突破该上限】——它并行的是扇出循环，
        // 而瓶颈在循环之外。真正有效的是按 IO 线程分组投递（见文件头 A1 说明与 LoopH），
        // 那把跨线程唤醒从 O(订阅者数) 降到 O(IO 线程数)。
        // 调大即可启用多分片并行扇出（需要同时把 fanoutThreads 设为 > 0）。
        size_t maxShards = 1;
        // 每多少个订阅者增加一个分片（分片数随房间规模阶梯增长：1/2/4/.../maxShards）
        size_t subsPerShard = 16;
        // 扇出线程池线程数。默认 0 = 不起线程池，全部内联扇出。
        //
        // 注意：多分片并行扇出（maxShards > 1 且 fanoutThreads > 0）目前是实验特性。
        // 单元测试（test/test_room_registry.cc）已验证其保序与并发正确性；
        // 但实测在 8 分片 / 8 线程下 CPU 占用升到约 3.3 核，吞吐仍与单分片持平
        // （同样受 drogon send 路径限制），只多付了记账与上下文切换成本，
        // 因此默认关闭，需要时用环境变量显式打开。
        size_t fanoutThreads = 0;
        // 订阅者数不超过该值时不惊动线程池，直接内联扇出
        size_t inlineMaxSubs = 32;
        // 单分片最大积压消息数
        size_t backlogPerShard = 4096;
        // 全局「在途投递份数」上限：所有目标 loop 队列里尚未执行的投递份数总和。
        //
        // 为什么需要它：backlogPerShard 只约束【分片队列】，而按 loop 分组投递之后
        // 真正的排队点是目标 loop 的 queueInLoop 队列 —— 那是无界的。慢消费者
        // （或卡住的 loop）会让已出队的批次一直堆在 loop 队列里，此时分片队列早已
        // 排空、backlogPerShard 永远不会触发，内存无界上涨且调用方收不到任何背压信号
        // （实测：backlog=4 时 publish 10000 条全部返回成功、dropped=0、连接收 0 条）。
        //
        // 语义：达到上限后 publish 直接返回 false（调用方计 ws_messages_dropped_total
        // 并回 503），【不改变入队顺序】—— 它只是提前拒绝，不做重排、不丢已入队消息，
        // 因此不影响保序。
        //
        // 默认值刻意取得很大（2^18）：它只是「已经在深度异常时」的安全阀，
        // 正常负载与压测都不会碰到。设 0 = 关闭该限制（退回旧行为）。
        // 可用 LOONG_WS_MAX_INFLIGHT 覆盖；生效值见 ws_fanout_inflight_limit，
        // 当前用量见 ws_fanout_inflight_deliveries（两者一起看才有意义）。
        size_t maxInFlightDeliveries = 1 << 18;
        // 单次连续扇出的消息批上限，超过则让出 worker 重排队，避免热点分片饿死其他房间
        size_t drainBatch = 32;
        // worker 在落眠前的无锁自旋轮数（每轮一条 cpuPause/yield 指令）。
        //
        // 为什么默认给这么大：热点分片会高频「空闲 → 忙」跳变，一旦落眠就要走
        // futex 唤醒，延迟在有负载的机器上会被放大到毫秒级，直接把延迟敏感型房间的
        // 吞吐打塌。自旋换的是「响应速度」，代价是空闲 worker 白烧 CPU。
        // 只有 fanoutThreads > 0（真起了线程池）时才有意义。
        //
        // 原先是 workerLoop() 里的 constexpr，调参要重新编译；提到这里后可经
        // LOONG_WS_WORKER_SPIN_ROUNDS 覆盖，便于压测 A/B。
        size_t workerSpinRounds = 4000;
    };

    explicit RoomRegistryT(Options opt = Options{}) : opt_(opt)
    {
        if (opt_.maxShards == 0)
        {
            opt_.maxShards = 1;
        }
        if (opt_.subsPerShard == 0)
        {
            opt_.subsPerShard = 16;
        }
        if (opt_.drainBatch == 0)
        {
            opt_.drainBatch = 1;
        }
        if (opt_.workerSpinRounds == 0)
        {
            // 0 会让 worker 完全不自旋、立刻落眠 —— 那是配置错误而不是意图，
            // 因为每次唤醒都要走一次 futex。按默认值兜底。
            opt_.workerSpinRounds = 4000;
        }

        // 没有线程池就谈不上并行扇出，分片数强制收敛为 1，避免白付分片记账开销
        if (opt_.fanoutThreads == 0)
        {
            opt_.maxShards = 1;
        }

        workers_.reserve(opt_.fanoutThreads);
        for (size_t i = 0; i < opt_.fanoutThreads; ++i)
        {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

    ~RoomRegistryT()
    {
        {
            std::lock_guard lock(poolMtx_);
            stop_ = true;
        }
        poolCv_.notify_all();
        for (auto& t : workers_)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
    }

    RoomRegistryT(const RoomRegistryT&) = delete;
    RoomRegistryT& operator=(const RoomRegistryT&) = delete;

    /**
     * @brief 订阅一个房间
     * @param loop 该连接所属的事件循环句柄。传入后，投递会在该 loop 的线程内完成
     *             （一次扇出对每个 loop 只唤醒一次）；不传则回退到直接 send。
     *             生产代码应从【连接的 IO 线程】里取
     *             trantor::EventLoop::getEventLoopOfCurrentThread() 传入
     *             —— drogon 的 handleNewConnection 正是在该线程内同步调用的。
     * @return 订阅 ID，用于 unsubscribe
     */
    SubscriberID subscribe(const std::string& room, ConnPtr conn, LoopH loop = LoopH{})
    {
        // reshape 前的「等排空」不能持 pubMtx 做无界自旋 —— 那会阻塞该房间的
        // 全部 publish 与 unsubscribe（只要有一个分片长时间 busy，整个房间就卡住）。
        // 这里改成「持锁检查 → 放锁让出 → 重新加锁复查」的循环；连续多轮未排空
        // 再退避一小段，避免纯自旋烧 CPU。
        constexpr int kMaxSpinRounds = 64;
        int spinRounds = 0;

        for (;;)
        {
            std::shared_ptr<Room> r = getOrCreateRoom(room);
            std::unique_lock pubLock(r->pubMtx);

            // 房间可能刚被并发退订判定为空并摘除。此时必须换一个房间重来，
            // 否则会往一个已经不在 rooms_ 里的孤儿 Room 塞成员（消息从此无处投递）。
            if (r->retired.load(std::memory_order_acquire))
            {
                pubLock.unlock();
                std::this_thread::yield();
                continue;
            }

            const size_t after = r->subCount.load(std::memory_order_relaxed) + 1;
            const size_t want = shardsFor(after);

            if (want != r->shards.size())
            {
                // 分片数跨越阶梯：必须等全部分片排空才能重建（见 reshapeLocked 注释）。
                // 锁内只做「检查」，等待放到锁外。
                bool drained = true;
                for (const auto& sh : r->shards)
                {
                    if (sh && sh->busy.load(std::memory_order_acquire))
                    {
                        drained = false;
                        break;
                    }
                }
                if (!drained)
                {
                    pubLock.unlock();
                    if (++spinRounds >= kMaxSpinRounds)
                    {
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                        spinRounds = 0;
                    }
                    else
                    {
                        std::this_thread::yield();
                    }
                    continue; // 重新取房间并复查
                }

                // 走安全重建协议（见 reshapeLocked 注释）
                const SubscriberID id = nextSubId_.fetch_add(1, std::memory_order_relaxed) + 1;
                reshapeLocked(*r, want, Entry{id, conn, loop});
                r->subCount.store(after, std::memory_order_relaxed);
                return id;
            }

            const SubscriberID id = nextSubId_.fetch_add(1, std::memory_order_relaxed) + 1;
            const size_t idx = static_cast<size_t>(id % want);

            // ── 强异常安全：先构造、后提交 ─────────────────────────────────
            // 顺序敏感：下列「构造」步骤（reserve / push_back / buildSnapshot）
            // 都可能因分配失败抛异常，必须在【改动任何权威状态之前】完成。
            // 原实现是「先 push 进 dist、再写 idToShard、最后 refreshSnapshot」，
            // 一旦中途抛异常就会留下「权威表已改、快照/计数未更新」的半提交状态，
            // 而外层拿到的 id_ 仍是 0（赋值没完成）→ unsubscribe(topic, 0) 清不掉，
            // 幽灵订阅者永久留在快照里继续收消息。
            //
            // 提交阶段（move 赋值 / 哈希插入 / 原子加）全部不会抛：
            // idToShard 先 reserve 好容量，插入不触发 rehash。
            r->idToShard.reserve(r->idToShard.size() + 1);
            std::vector<Entry> nextBucket = r->dist[idx];
            nextBucket.reserve(r->dist[idx].size() + 1);
            nextBucket.push_back(Entry{id, conn, loop});
            auto nextSnap = buildSnapshot(nextBucket); // 唯一可能抛异常的一步

            const bool wasEmpty = r->dist[idx].empty();
            r->dist[idx] = std::move(nextBucket);
            r->idToShard[id] = idx;
            r->shards[idx] = refreshSnapshotWith(r->shards[idx], std::move(nextSnap));
            if (wasEmpty)
            {
                r->nonEmptyShards.fetch_add(1, std::memory_order_relaxed);
            }
            r->subCount.store(after, std::memory_order_relaxed);
            return id;
        }
    }

    /**
     * @brief 退订。幂等：重复退订或不存在的 ID 均安全返回。
     */
    void unsubscribe(const std::string& room, SubscriberID id)
    {
        std::shared_ptr<Room> r;
        {
            std::shared_lock roomsLock(roomsMtx_);
            if (const auto it = rooms_.find(room); it != rooms_.end())
            {
                r = it->second;
            }
        }
        if (!r)
        {
            return;
        }

        std::lock_guard pubLock(r->pubMtx);
        const auto it = r->idToShard.find(id);
        if (it == r->idToShard.end())
        {
            return; // 幂等：重复退订 / 不存在的 ID 直接放过
        }
        const size_t idx = it->second;

        // ── 强异常安全：先构造、后提交（理由同 subscribe）─────────────────
        // 原实现是「先从 dist/idToShard 里 erase、再 refreshSnapshot」，
        // 而 refreshSnapshot 内部要分配。分配失败时异常逃出，结果是：
        // 权威成员表已经删了，但 subCount 没减、retired 没置、旧快照里
        // 仍然含这个连接 —— 已退订的连接继续收消息，且房间永远不会被回收
        // （实测：分配失败一次后，全部退订完毕 rooms 仍为 1、subscribers 仍为 1）。
        // 改成「构造阶段可能抛、提交阶段不会抛」之后，失败时房间保持原样，
        // 调用方可以安全重试。
        std::vector<Entry> remaining;
        {
            const auto& bucket = r->dist[idx];
            remaining.reserve(bucket.size());
            for (const auto& e : bucket)
            {
                if (e.id != id)
                {
                    remaining.push_back(e);
                }
            }
        }
        auto nextSnap = buildSnapshot(remaining); // 唯一可能抛异常的一步
        std::shared_ptr<Shard> shard = r->shards[idx];
        if (!shard)
        {
            shard = std::make_shared<Shard>(); // 同样在提交前完成
        }

        // ── 提交阶段：以下操作都不会抛 ──────────────────────────────────
        r->dist[idx] = std::move(remaining);
        r->idToShard.erase(it);
        {
            std::lock_guard shardLock(shard->mtx);
            shard->snap = std::move(nextSnap);
        }
        r->shards[idx] = std::move(shard);
        if (r->dist[idx].empty())
        {
            r->nonEmptyShards.fetch_sub(1, std::memory_order_relaxed);
        }

        if (r->subCount.fetch_sub(1, std::memory_order_relaxed) != 1)
        {
            return; // 房间里还有人，不回收
        }

        // 房间空了：置 retired 并摘除。
        // 这里在持有 pubMtx 的同时取 roomsMtx_ 独占锁 —— 全局锁序恒为
        // 「先 roomsMtx_ 后释放，再取 pubMtx」或「持 pubMtx 时取 roomsMtx_」，
        // 不存在反序获取，因此不会死锁。
        r->retired.store(true, std::memory_order_release);
        std::unique_lock roomsLock(roomsMtx_);
        if (const auto mit = rooms_.find(room); mit != rooms_.end() && mit->second == r)
        {
            rooms_.erase(mit);
            // 记下序号水位线，供同名房间重建时续用（见 roomSeqWatermark_）。
            // 这里仍持 pubMtx（pubLock 在外层作用域），读 r->seq 是安全的。
            if (roomSeqWatermark_.size() >= kMaxSeqWatermarks)
            {
                roomSeqWatermark_.clear();
            }
            roomSeqWatermark_[room] = r->seq;
        }
    }

    /**
     * @brief 取房间句柄，供调用方缓存后随 publish 传回（见 publish 的 hint 参数）。
     *
     * 典型用法：订阅成功后取一次存进订阅者对象，此后该订阅者每次发布都带上传回的
     * 句柄，从而完全绕开全局 roomsMtx_ 共享锁与房间名哈希 —— 全局表锁的访问频次
     * 从「每条消息一次」降到「每次订阅一次」。
     *
     * 房间被回收（retired）后句柄自动失效，publish 会回退到按名字查表，因此
     * 无需在退订时主动清理这个缓存（缓存一个已失效的句柄不会出错，只是慢一次）。
     *
     * @return 房间不存在时返回空句柄。
     */
    RoomHandle acquireRoomHandle(const std::string& room) const
    {
        std::shared_lock lock(roomsMtx_);
        const auto it = rooms_.find(room);
        return it == rooms_.end() ? RoomHandle{} : it->second;
    }

    /**
     * @brief 向房间发布一条消息（payload 已序列化完成）
     * @param outSeq 非空时写回本条消息的房间级单调序号（见下方 publishShared 的说明）。
     *               写回 0 表示「本次没有分配序号」——即房间不存在、无订阅者，
     *               或消息被背压拒绝。调用方据此决定要不要带序号广播给集群。
     * @return false 表示分片积压达上限、消息被丢弃（调用方需计数并告知客户端）
     */
    bool publish(const std::string& room, const std::string& payload, const RoomHandle& hint = {},
                 uint64_t* outSeq = nullptr)
    {
        return publishShared(room, std::make_shared<const std::string>(payload), hint, outSeq);
    }

    bool publish(const std::string& room, std::string&& payload, const RoomHandle& hint = {},
                 uint64_t* outSeq = nullptr)
    {
        return publishShared(room, std::make_shared<const std::string>(std::move(payload)), hint,
                             outSeq);
    }

    // ── 房间级单调序号（跨实例保序的「证人」）──────────────────────────────────
    //
    // 为什么需要：本工程的保序是【管道保序】——顺序权威来自「房间 pubMtx → 分片 FIFO
    // → 目标 loop 的 queueInLoop FIFO」这条链，消息体里不带序号。它在「单实例 +
    // 单一路径 + 在线实时」下成立，但跨实例广播时会出现一个空洞：
    //   M1 在实例 A 发、M2 在实例 B 发 ⇒ A 的订阅者走「M1 本地 + M2 总线」，
    //   B 的订阅者走「M2 本地 + M1 总线」，两条路径延迟不同 ⇒ 两侧可能看到不同顺序。
    //
    // 序号在【同一把 pubMtx 内】分配，因此「序号的大小顺序」与「入队顺序」严格一致。
    // 接收端按 (发送实例, 房间) 记 lastSeq，即可发现：
    //   · 缺口：序号跳跃 ⇒ 总线链路上丢了消息（Redis 丢包 / 实例崩溃 / 订阅中断）
    //   · 回退：序号小于等于已见最大值 ⇒ 同一来源的消息被乱序投递
    //
    // ⚠️ 能力边界（必须诚实声明）：单靠「每来源一个计数器」【检测不到】上面那个
    //    跨来源乱序 —— 两个来源的计数器之间没有可比性。要真正消除它需要一个
    //    全局唯一的定序权威（例如 Redis INCR 或专门的定序实例），代价是给发布
    //    热路径加一次网络往返。本函数只提供【可观测性】：让「跨实例到底有没有
    //    丢消息/乱序」从「无从判断」变成「有数可查」。
    //
    // 序号从 1 开始（0 保留给「未分配」），被背压拒绝的消息【不占号】——
    // 否则接收端会把「本地根本没发出去的那条」误报成「传输中丢了一条」。
    bool publishShared(const std::string& room, std::shared_ptr<const std::string> payload,
                       const RoomHandle& hint = {}, uint64_t* outSeq = nullptr)
    {
        if (outSeq != nullptr)
        {
            *outSeq = 0;
        }
        // ── 全局在途投递上限：唯一能感知「下游已堵死」的背压信号 ───────────────
        //
        // 放在最前面（房间表锁之前）：该判定完全不依赖房间状态，提前拒绝更便宜。
        // 这里只做「拒绝」，不重排、不丢弃任何已入队消息 ⇒ 不影响保序。
        // 说明见 Options::maxInFlightDeliveries。
        if (opt_.maxInFlightDeliveries != 0 &&
            inFlight_->load(std::memory_order_relaxed) >= opt_.maxInFlightDeliveries)
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            inflightRejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        std::shared_ptr<Room> r;

        // ── 快路径：调用方缓存了房间句柄 → 直接复用，不过全局表锁 ─────────────
        //
        // retired 的读判是安全的：退订摘除房间的顺序恒为「先 retired.store(true)，
        // 再 rooms_.erase」，所以读到 !retired 就蕴含「这个 Room 仍是 rooms_ 里
        // 的那一份」。反过来若读到 retired，则可能已经有同名的新房间被建出来，
        // 必须回退查表 —— 否则消息会被投进一个成员已清空的孤儿房间（静默丢消息）。
        //
        // ⚠️ 前置条件：hint 必须与 room 指向同一房间。若 hint 有效却属于别的房间，
        //    消息会被投到 hint 的房间去（此时 room 参数被忽略）。调用方务必把
        //    「同一个订阅者身上的 topic_」和「它的 room_」成对传进来。
        if (hint && !hint->retired.load(std::memory_order_acquire))
        {
            r = hint;
        }
        else
        {
            std::shared_lock roomsLock(roomsMtx_);
            if (const auto it = rooms_.find(room); it != rooms_.end())
            {
                r = it->second;
            }
        }
        if (!r || !payload)
        {
            return true; // 无订阅者：与 drogon 旧行为一致，直接丢弃
        }

        // 小房间 / 未启用线程池：内联扇出，不惊动线程池
        const bool inlineMode =
            workers_.empty() || r->subCount.load(std::memory_order_relaxed) <= opt_.inlineMaxSubs;

        std::vector<std::shared_ptr<Shard>> needWake;
        // 预分配容量：needWake 的 push_back 发生在 busy.exchange(true)【之后】，
        // 一旦在那里分配失败，就会留下「busy=true 但池里没有该分片」的状态 ——
        // 该分片从此无人抽干（后续消息只入队不投递）。先 reserve 掉这种可能。
        // 上限就是非空分片数，容量很小。
        needWake.reserve(r->dist.size());
        // 本条的序号。0 = 未分配（被拒 / 无订阅者），见下方提交点。
        uint64_t assignedSeq = 0;
        {
            // 房间级发布锁：消息顺序的唯一来源。只做「入队」，不做扇出。
            std::lock_guard pubLock(r->pubMtx);

            // ── 快路径：恰好一个非空分片（小房间的常态）────────────────────
            // 两阶段协议要遍历 dist 两次、对同一个分片加解锁两次。但「全有或全无」
            // 这个约束在只有一个非空分片时是平凡的 —— 单分片的「检查」与「入队」
            // 可以在同一把分片锁里一次做完，省掉一遍遍历和一次 mutex 往返。
            //
            // 门控读的是 nonEmptyShards：它在 pubMtx 内是精确值，subscribe /
            // unsubscribe / reshapeLocked 三处都持同一把房间锁维护它，不存在读到
            // 中间态的可能（因此不会出现「实际两个分片非空、却按一个分片投递」
            // 这种静默漏投）。分片数 k=1 的房间恒走这条路。
            if (r->nonEmptyShards.load(std::memory_order_relaxed) == 1)
            {
                for (size_t i = 0; i < r->dist.size(); ++i)
                {
                    if (r->dist[i].empty())
                    {
                        continue;
                    }
                    const std::shared_ptr<Shard>& sh = r->shards[i];
                    {
                        std::lock_guard shardLock(sh->mtx);
                        if (sh->queue.size() >= opt_.backlogPerShard)
                        {
                            // 整条消息按一次丢弃计数（与两阶段路径口径一致）
                            dropped_.fetch_add(1, std::memory_order_relaxed);
                            return false;
                        }
                        sh->queue.push_back(payload);
                        // 门铃：仅在「空闲 → 忙」跳变时入池，热点分片不会每条消息都惊动线程池。
                        // exchange 与 drain 的 busy=false 都在分片锁内完成，不会丢唤醒。
                        if (!sh->busy.exchange(true, std::memory_order_acq_rel))
                        {
                            needWake.push_back(sh);
                        }
                    }
                    break; // 有且仅有一个非空分片
                }
            }
            else
            {
                // ── 第一阶段：预检查（全有或全无）──────────────────────────────
                // 只要有一个非空分片已满，整条消息就都不入队并返回 false。
                //
                // 原实现是「逐分片判断、满的那个 continue」，于是过载时会出现
                // 「一部分订阅者已收到、另一部分被丢」——调用方只能回一个 503，
                // 客户端重试后已收到的人又会看到重复消息。多分片时必然发生。
                //
                // 同房间的所有 publish 都被 pubMtx 串行化，所以「预检查通过」到
                // 「入队」之间不会有别的 publish 插入；drain 只会让队列更空、
                // 不会让它变满，因此预检查的结论在第二阶段依然成立。
                for (size_t i = 0; i < r->dist.size(); ++i)
                {
                    if (r->dist[i].empty())
                    {
                        continue;
                    }
                    const std::shared_ptr<Shard>& sh = r->shards[i];
                    std::lock_guard shardLock(sh->mtx);
                    if (sh->queue.size() >= opt_.backlogPerShard)
                    {
                        // 整条消息按一次丢弃计数（而不是每个满分片各计一次），
                        // 这样 dropped_ 与「被拒绝的 publish 次数」严格一致。
                        dropped_.fetch_add(1, std::memory_order_relaxed);
                        return false;
                    }
                }

                // ── 第二阶段：整条消息入队 ────────────────────────────────────
                for (size_t i = 0; i < r->dist.size(); ++i)
                {
                    if (r->dist[i].empty())
                    {
                        continue; // 空槽位不建对象也不发布，小房间不为用不到的分片买单
                    }
                    const std::shared_ptr<Shard>& sh = r->shards[i];
                    std::lock_guard shardLock(sh->mtx);
                    sh->queue.push_back(payload);
                    // 门铃：仅在「空闲 → 忙」跳变时入池，热点分片不会每条消息都惊动线程池。
                    // exchange 与 drain 的 busy=false 都在分片锁内完成，不会丢唤醒。
                    if (!sh->busy.exchange(true, std::memory_order_acq_rel))
                    {
                        needWake.push_back(sh);
                    }
                }
            }

            // 在 pubMtx 内入池（内联模式除外）：保证「busy == true ⟹ 分片已在池中或正在被抽」，
            // 让 reshape 的排空等待一定有进展，不会等一个没人抽的分片。
            if (!inlineMode)
            {
                for (const auto& sh : needWake)
                {
                    enqueueReady(sh);
                }
            }

            // ── 提交点：分配房间级序号 ──────────────────────────────────────────
            //
            // 位置是刻意选在这里（所有 return false 之后、同一把 pubMtx 之内）：
            //   ① 同一把锁 ⇒ 序号大小顺序 == 入队顺序，接收端才能拿它做缺口检测；
            //   ② 被背压拒绝的消息不占号 ⇒ 不会把「本地没发出去」误报成「传输丢了」。
            // r->seq 是普通 uint64_t（不是原子量）：唯一写入点就在这里，且恒在
            // pubMtx 内；读取点（退订时存水位线）同样持 pubMtx。
            assignedSeq = ++r->seq;
        }

        if (outSeq != nullptr)
        {
            *outSeq = assignedSeq;
        }

        if (needWake.empty())
        {
            return true; // 已有线程在抽这些分片，它会在循环里取到本条消息
        }

        if (inlineMode)
        {
            // 内联抽干：不占线程池槽位。同一分片不会被两个线程同时抽，
            // 因为 busy 标志保证了「同一时刻只有一个抽干者」。
            for (const auto& sh : needWake)
            {
                drainShard(sh, false);
            }
        }
        return true;
    }

    // ---- 观测 ----

    size_t activeRooms() const
    {
        std::shared_lock lock(roomsMtx_);
        return rooms_.size();
    }

    size_t activeShards() const
    {
        std::shared_lock lock(roomsMtx_);
        size_t n = 0;
        for (const auto& [name, r] : rooms_)
        {
            (void)name;
            n += r->nonEmptyShards.load(std::memory_order_relaxed);
        }
        return n;
    }

    size_t droppedCount() const noexcept
    {
        return dropped_.load(std::memory_order_relaxed);
    }

    size_t subscribersIn(const std::string& room) const
    {
        std::shared_lock lock(roomsMtx_);
        const auto it = rooms_.find(room);
        return it == rooms_.end() ? 0 : it->second->subCount.load(std::memory_order_relaxed);
    }

    const Options& options() const noexcept
    {
        return opt_;
    }

    // 一次性快照（单锁单遍），供周期性指标采集使用。
    // 分开调 activeRooms()/activeShards()/totalSubscribers() 会各加一次锁、
    // 且几次读数之间房间集合可能已变，得到的是一组自相矛盾的数。
    struct Stats
    {
        size_t rooms = 0;              // 活跃房间数
        size_t shards = 0;             // 全体房间的「非空分片」总数
        size_t subscribers = 0;        // 全体房间的订阅者总数
        size_t maxRoomSubscribers = 0; // 最大单房间订阅者数（扇出成本的关键指标）
    };

    Stats stats() const
    {
        std::shared_lock lock(roomsMtx_);
        Stats s;
        s.rooms = rooms_.size();
        for (const auto& [name, r] : rooms_)
        {
            (void)name;
            s.shards += r->nonEmptyShards.load(std::memory_order_relaxed);
            const size_t n = r->subCount.load(std::memory_order_relaxed);
            s.subscribers += n;
            if (n > s.maxRoomSubscribers)
            {
                s.maxRoomSubscribers = n;
            }
        }
        return s;
    }

    // 分组投递的效果观测 —— 用来在生产上确认 A1 的快速路径真的生效了
    // （否则又会变成「优化了但没人知道有没有生效」的盲区）。
    //
    // 判读方式：一次房间扇出共 M 条消息、N 个订阅者、K 个 IO 线程时，
    //   快速路径生效 → crossThreadBatches ≈ M×K 量级，inLoopDeliveries ≈ M×N 量级；
    //   分组没生效（loop 抓错 / 回退）→ crossThreadBatches 会与 M×N 同量级，
    //   且 inLoopDeliveries 接近 0。
    size_t inLoopDeliveries() const noexcept
    {
        return inLoopDeliveries_.load(std::memory_order_relaxed);
    }

    size_t crossThreadBatches() const noexcept
    {
        return crossThreadBatches_.load(std::memory_order_relaxed);
    }

    // 跨线程批次里「裹了多少份」—— batches 是唤醒次数，这个是实际投递份数。
    // 必须分开记：只知道 batches 无法还原投递总量（一个 batch 裹几个连接
    // 只有运行时才知道），线上就少了一个「到底投出去多少」的判据。
    size_t crossThreadDeliveries() const noexcept
    {
        return crossThreadDeliveries_.load(std::memory_order_relaxed);
    }

    // 投递阶段被捕获的异常次数（loop 队列分配失败、连接 send 抛异常等）。
    //
    // 为什么必须暴露：这几处原先没有 catch，异常会直接穿出 drainer ——
    // 线程池模式下等于 std::terminate，内联模式下等于 publish 抛给业务层。
    // 现在改成「吞掉 + 恢复 busy + 计数」，但如果看不见这个数，
    // 「消息偶发丢失」就又变成了无迹可查的静默故障。
    // 判读：非 0 即说明投递路径发生过异常，应查日志与内存压力。
    size_t fanoutExceptions() const noexcept
    {
        return fanoutExceptions_.load(std::memory_order_relaxed);
    }

    // 当前在途投递份数（瞬时值）。它是判断「下游 loop 是否已经堵死」的唯一直接证据：
    // 持续贴近 maxInFlightDeliveries 就说明消费端跟不上生产端。
    size_t inFlightDeliveries() const noexcept
    {
        return inFlight_->load(std::memory_order_relaxed);
    }

    // 因在途上限被拒的 publish 次数（累计）。
    size_t inflightRejectedCount() const noexcept
    {
        return inflightRejected_.load(std::memory_order_relaxed);
    }

  private:
    struct Entry
    {
        SubscriberID id{0};
        ConnPtr conn;
        // 该连接所属的事件循环（订阅时抓取，终身不变）
        LoopH loop{};
    };

    // 投递计划的一个分组：同一 loop 上的若干连接。
    // conns 用 shared_ptr<const> 共享，扇出时每个跨线程批次只拷贝一个指针，
    // 不产生「按连接逐个拷贝」的开销。
    struct Group
    {
        LoopH loop;
        std::shared_ptr<const std::vector<ConnPtr>> conns;
    };

    // 分组后的投递计划（只读，扇出热路径直接遍历）
    using Snapshot = std::vector<Group>;

    struct Shard
    {
        // 一把锁同时保护 queue / snap / busy 的写入。
        // 扇出（send）刻意放在锁外，因此不会阻塞发布方入队。
        std::mutex mtx;
        std::deque<std::shared_ptr<const std::string>> queue;
        // copy-on-write 快照：扇出线程每条消息只拷贝一次 shared_ptr，无 O(N) 引用计数风暴
        std::shared_ptr<const Snapshot> snap = std::make_shared<const Snapshot>();
        std::atomic<bool> busy{false};
    };

    struct Room
    {
        // 房间级发布锁 —— 消息顺序的唯一来源
        std::mutex pubMtx;
        // 固定槽位，惰性创建；dist[i] 是分片 i 的权威成员表，成员永不迁移
        std::vector<std::shared_ptr<Shard>> shards;
        std::vector<std::vector<Entry>> dist;
        phmap::flat_hash_map<SubscriberID, size_t> idToShard;
        std::atomic<size_t> subCount{0};
        std::atomic<size_t> nonEmptyShards{0};
        // 已被回收标记：防止退订摘除房间与并发订阅之间出现「孤儿房间」
        std::atomic<bool> retired{false};
        // 房间级单调序号（从 1 开始，0 = 未分配）。只在 pubMtx 内读写。
        // 房间被回收时它的当前值会被记进 roomSeqWatermark_，同名房间重建时从这里
        // 续上 —— 否则重建后序号从 1 重来，接收端会把它当成「大规模乱序/回退」误报。
        uint64_t seq{0};
    };

    // 把「分片成员表」编成「按 loop 分组的投递计划」。
    // 只在订阅 / 退订 / reshape 时执行（O(该分片成员数)），扇出热路径只读不建。
    //
    // 分片成员数通常远小于全局订阅者数，且 loop 种类很少（≤ IO 线程数），
    // 所以这里用线性查找而非哈希表 —— 避免每次重建都付一次哈希表构造成本。
    static std::shared_ptr<const Snapshot> buildSnapshot(const std::vector<Entry>& bucket)
    {
        std::vector<std::pair<LoopH, std::vector<ConnPtr>>> tmp;
        tmp.reserve(bucket.size() > 16 ? 16 : bucket.size());
        for (const auto& e : bucket)
        {
            if (!e.conn)
            {
                continue; // 空连接不参与投递，也不该占一个分组
            }
            auto it = std::find_if(tmp.begin(), tmp.end(),
                                   [&e](const auto& p) { return p.first == e.loop; });
            if (it == tmp.end())
            {
                tmp.emplace_back(e.loop, std::vector<ConnPtr>{});
                it = std::prev(tmp.end());
            }
            it->second.push_back(e.conn);
        }

        auto snap = std::make_shared<Snapshot>();
        snap->reserve(tmp.size());
        for (auto& [loop, conns] : tmp)
        {
            snap->push_back(
                Group{loop, std::make_shared<const std::vector<ConnPtr>>(std::move(conns))});
        }
        return snap;
    }

    // 重建某个分片的 COW 快照。O(该分片成员数)，不触发 O(N) 全量拷贝。
    static std::shared_ptr<Shard> refreshSnapshot(const std::shared_ptr<Shard>& sh,
                                                  const std::vector<Entry>& bucket)
    {
        auto next = buildSnapshot(bucket);
        if (!sh)
        {
            auto created = std::make_shared<Shard>();
            created->snap = std::move(next);
            return created;
        }
        {
            std::lock_guard lock(sh->mtx);
            sh->snap = std::move(next);
        }
        return sh;
    }

    // 同上，但快照已在【外部构造完成】。
    //
    // 存在的理由：强异常安全要求「先构造、后提交」——构造（分配）可能抛异常，
    // 必须发生在改动权威状态之前；而提交这一步本身不能抛。把两件事拆开之后，
    // subscribe / unsubscribe 就能做到「要么全改，要么一点没改」。
    static std::shared_ptr<Shard> refreshSnapshotWith(const std::shared_ptr<Shard>& sh,
                                                      std::shared_ptr<const Snapshot> next)
    {
        if (!sh)
        {
            auto created = std::make_shared<Shard>();
            created->snap = std::move(next);
            return created;
        }
        {
            std::lock_guard lock(sh->mtx);
            sh->snap = std::move(next);
        }
        return sh;
    }

    std::shared_ptr<Room> getOrCreateRoom(const std::string& room)
    {
        {
            std::shared_lock lock(roomsMtx_);
            if (const auto it = rooms_.find(room); it != rooms_.end())
            {
                return it->second;
            }
        }
        // 插入必须走独占锁：共享锁下写 phmap 会与并发的 find 相互踩踏（哈希表损坏）
        std::unique_lock lock(roomsMtx_);
        if (const auto it = rooms_.find(room); it != rooms_.end())
        {
            return it->second;
        }
        auto r = std::make_shared<Room>();
        // 同名房间重建时把序号续上（见 roomSeqWatermark_ 的说明）。
        // 取出即从水位线表里移除：房间活着的时候序号在 Room 里，不需要两份。
        if (const auto wit = roomSeqWatermark_.find(room); wit != roomSeqWatermark_.end())
        {
            r->seq = wit->second;
            roomSeqWatermark_.erase(wit);
        }
        // shards/dist 刻意留空：首次订阅时由 reshapeLocked 按规模一次性建好
        rooms_[room] = r;
        return r;
    }

    // 分片数随房间规模阶梯增长：1 / 2 / 4 / ... / maxShards。
    // 只增不减 —— 缩容需要在成员迁移时额外处理顺序，收益不值这个复杂度；
    // 房间在最后一个订阅者离开时会被整体回收，因此不存在长期占用。
    size_t shardsFor(size_t subs) const
    {
        size_t k = 1;
        while (k < opt_.maxShards && subs >= k * opt_.subsPerShard)
        {
            k <<= 1;
        }
        return k;
    }

    static std::shared_ptr<Shard> makeShard(const std::vector<Entry>& bucket)
    {
        auto sh = std::make_shared<Shard>();
        sh->snap = buildSnapshot(bucket);
        return sh;
    }

    // 分片数变化时的安全重建协议。调用方必须持有 r.pubMtx，
    // 且【必须先确认所有分片已排空（busy == false）】—— 该确认由 subscribe 在
    // 锁内完成、等待在锁外进行，避免持锁自旋把整个房间的 publish 卡住。
    //
    // 核心约束：只有当【全体订阅者都收到了截止此刻的全部消息】时，重新分片才是安全的。
    // 否则会出现「某人已经收过、某人还没收」的错位 —— 那是重复投递的来源
    // （早期版本把各分片残留 payload 统一搬运再广播，实测必然产生重复）。
    //
    // 「排空」在持 pubMtx 时是免费可得的：调用方持 pubMtx，不会再有新消息入队；
    // 又因为 publishShared 在 pubMtx 内就把分片入池，所以 busy == true 一定意味着
    // 「该分片在池中或正在被抽」，等待 busy == false 必然收敛，且收敛时队列已空。
    // 换言之：持 pubMtx 时 busy == false ⟺ 队列为空且无在途扇出。
    //
    // 注意分组投递（A1）之后 busy == false 的语义收紧为「已把全部消息【派发】到目标
    // loop」，而不是「已送达」。这不影响本函数的正确性：reshape 只在既有订阅者之间
    // 重新分配分片，不增删成员，而每个已派发批次的连接列表是快照里的不可变集合，
    // 所以即使批次还在目标 loop 队列里等待执行，它的收件人集合也不会因 reshape 而改变。
    void reshapeLocked(Room& r, size_t newK, const Entry& extra)
    {
        std::vector<Entry> all;
        all.reserve(r.subCount.load(std::memory_order_relaxed) + 1);
        for (const auto& bucket : r.dist)
        {
            all.insert(all.end(), bucket.begin(), bucket.end());
        }
        all.push_back(extra);

        r.shards.assign(newK, nullptr);
        r.dist.assign(newK, {});
        r.idToShard.clear();
        r.idToShard.reserve(all.size());
        r.nonEmptyShards.store(0, std::memory_order_relaxed);
        for (const auto& e : all)
        {
            const size_t idx = static_cast<size_t>(e.id % newK);
            r.dist[idx].push_back(e);
            r.idToShard[e.id] = idx;
        }
        for (size_t i = 0; i < newK; ++i)
        {
            r.shards[i] = makeShard(r.dist[i]);
            if (!r.dist[i].empty())
            {
                r.nonEmptyShards.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // ---------------- 扇出线程池 ----------------

    void enqueueReady(const std::shared_ptr<Shard>& sh)
    {
        {
            std::lock_guard lock(poolMtx_);
            ready_.push_back(sh);
            readyCount_.fetch_add(1, std::memory_order_release);
        }
        if (parked_.load(std::memory_order_relaxed) > 0)
        {
            poolCv_.notify_one();
        }
    }

    // 自旋提示：热点分片会高频「空闲 → 忙」跳变，落眠后走 futex 唤醒的延迟
    // 在机器有负载时会被放大到毫秒级，直接把延迟敏感型房间的吞吐打塌。
    static void cpuPause() noexcept
    {
#if defined(__aarch64__) || defined(_M_ARM64)
        asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
        asm volatile("pause" ::: "memory");
#else
        std::this_thread::yield();
#endif
    }

    void workerLoop()
    {
        // 自旋轮数是可调项（Options::workerSpinRounds，可用环境变量覆盖）：
        // 热点分片靠它避免 futex 唤醒延迟，空闲时靠它白烧 CPU，取舍随负载而定。
        const int kSpinRounds = static_cast<int>(opt_.workerSpinRounds);

        for (;;)
        {
            std::shared_ptr<Shard> sh;
            bool got = false;

            // 阶段一：无锁自旋。就绪计数为 0 时只做 pause，不碰锁、不落眠。
            for (int spin = 0; spin < kSpinRounds; ++spin)
            {
                if (readyCount_.load(std::memory_order_acquire) != 0)
                {
                    std::lock_guard lock(poolMtx_);
                    if (!ready_.empty())
                    {
                        sh = std::move(ready_.front());
                        ready_.pop_front();
                        readyCount_.fetch_sub(1, std::memory_order_relaxed);
                        got = true;
                    }
                    break;
                }
                cpuPause();
            }

            // 阶段二：确实没活可干才落眠
            if (!got)
            {
                std::unique_lock lock(poolMtx_);
                ++parked_;
                poolCv_.wait(lock, [this] { return stop_ || !ready_.empty(); });
                --parked_;
                if (ready_.empty())
                {
                    if (stop_)
                    {
                        return;
                    }
                    continue;
                }
                sh = std::move(ready_.front());
                ready_.pop_front();
                readyCount_.fetch_sub(1, std::memory_order_relaxed);
            }

            drainShard(sh, true);
        }
    }

    // 抽干一个分片。yielding 为 true 时按批让出 worker，避免热点分片饿死其他房间。
    void drainShard(const std::shared_ptr<Shard>& sh, bool yielding)
    {
        size_t processed = 0;
        for (;;)
        {
            std::shared_ptr<const std::string> payload;
            std::shared_ptr<const Snapshot> snap;
            bool requeue = false;
            {
                std::lock_guard lock(sh->mtx);
                if (sh->queue.empty())
                {
                    // 必须在锁内置 busy=false：与发布方的 exchange(true) 串行化，杜绝丢唤醒
                    sh->busy.store(false, std::memory_order_release);
                    return;
                }
                payload = std::move(sh->queue.front());
                sh->queue.pop_front();
                snap = sh->snap;
                if (yielding && ++processed >= opt_.drainBatch && !sh->queue.empty())
                {
                    requeue = true;
                }
            }

            // 先投递本条，再决定是否让出 worker —— 顺序不能颠倒，否则本条会被丢掉
            try
            {
                fanOutToSnapshot(*snap, payload);
            }
            catch (...)
            {
                // ⚠️ 投递阶段（loop 队列分配等）抛异常【绝不能】让它逃出去：
                //   ① 逃出 workerLoop → 异常穿出 std::thread → std::terminate 整个进程；
                //   ② 逃出内联路径 → publish 把异常抛给业务调用方（handleNewMessage）。
                // 更重要的是必须【恢复 busy】—— 原实现让它保持 true，于是该分片
                // 再也不会被抽干：后续消息只入队不投递，直到队列堆满后开始被拒
                // （实测：dispatch 抛一次之后，分片永久卡死）。
                // 本条已出队的消息无法重投（payload 已从队列取走），计一次异常；
                // 队列里剩下的消息在 busy 复位后由下面这段重新唤醒继续投递。
                fanoutExceptions_.fetch_add(1, std::memory_order_relaxed);
                bool more = false;
                {
                    std::lock_guard lock(sh->mtx);
                    sh->busy.store(false, std::memory_order_release);
                    more = !sh->queue.empty();
                }
                if (more && yielding)
                {
                    // 池化模式：立刻重新占住 busy 并送回池中。否则剩余消息要等到
                    // 下一次 publish 才会被唤醒（延迟不可控）。
                    {
                        std::lock_guard lock(sh->mtx);
                        sh->busy.store(true, std::memory_order_release);
                    }
                    enqueueReady(sh);
                }
                return;
            }

            if (requeue)
            {
                // busy 保持 true，重新排队继续抽；同一分片不会被并发扇出
                enqueueReady(sh);
                return;
            }
        }
    }

    // 真正的扇出：锁外执行。
    //
    // 逐组投递：命中本线程的组直接 send（零跨线程开销）；跨线程的组只唤醒一次、
    // 携带整批连接，实际 send 在目标 loop 的线程内完成 → 走 drogon/trantor 的
    // isInLoopThread() 快路径。唤醒次数由 O(订阅者数) 降为 O(目标 loop 数)。
    void fanOutToSnapshot(const Snapshot& snap,
                          const std::shared_ptr<const std::string>& payload) const
    {
        for (const auto& g : snap)
        {
            if (!g.conns || g.conns->empty())
            {
                continue;
            }

            // ── 为什么「本线程的组」也一律排队，而不是直接 send ──────────────
            // 直投与排队混用会破坏「同一订阅者看到的消息顺序」：
            //   抽干线程 = 消息发布者所在的 IO 线程（默认 fanoutThreads == 0 时全内联抽干）。
            //   M1 由 loop A 抽干 → 对 loop B 的组走 queueInLoop（排队，尚未执行）；
            //   M2 由 loop B 抽干 → 对 loop B 的组若走直投，就会抢在 M1 那个批次之前送达。
            //   于是 loop B 上的订阅者先收到 M2、后收到 M1 —— 顺序颠倒。
            // 统一排队后，每个目标 loop 只有「一条 FIFO 入口」，入队顺序即投递顺序。
            // 代价：本线程的组多一次本地入队（无系统调用）；跨线程唤醒次数仍是
            // O(loop 数)，A1 优化的收益（O(订阅者数) → O(loop 数) 次唤醒）不受影响。
            //
            // loop 句柄无效时无处可排队（单测 / 未提供 loop 的调用方），退化为直投，
            // 语义与优化前一致。
            if (!g.loop.valid())
            {
                const std::string_view view(*payload);
                for (const auto& c : *g.conns)
                {
                    c->send(view);
                }
                inLoopDeliveries_.fetch_add(g.conns->size(), std::memory_order_relaxed);
                continue;
            }

            const bool sameThread = g.loop.isCurrentThread();
            // 只捕获两个 shared_ptr（payload + 连接组），不复制连接列表本身。
            // 连接对象因此至少活到这个批次被执行完，之后才可能析构。
            //
            // 在途计数：必须【先加后派发】。反过来的话，「已入队但还没计数」的那个
            // 窗口里上游会以为下游还空着，正好在最该背压的时刻放行。执行完在
            // lambda 里减回去 —— 份数在派发时就确定，批次一旦入队必然整批执行。
            const size_t batch = g.conns->size();
            auto inFlight = inFlight_;
            inFlight->fetch_add(batch, std::memory_order_relaxed);
            try
            {
                g.loop.dispatch([payload, conns = g.conns, inFlight, batch] {
                    const std::string_view view(*payload);
                    for (const auto& c : *conns)
                    {
                        c->send(view);
                    }
                    inFlight->fetch_sub(batch, std::memory_order_relaxed);
                });
            }
            catch (...)
            {
                // 派发失败 ⇒ 这批永远不会被执行，必须立刻把计数还回去。否则在途计数
                // 只增不减（每次投递失败漏一批），最终把整个进程钉死在上限上 ——
                // 那是比「一条消息没投出去」严重得多的故障。
                inFlight->fetch_sub(batch, std::memory_order_relaxed);
                throw;
            }

            if (sameThread)
            {
                // 指标语义保持不变：这一组原本走直投，现在只是改为经本线程队列。
                inLoopDeliveries_.fetch_add(g.conns->size(), std::memory_order_relaxed);
            }
            else
            {
                crossThreadBatches_.fetch_add(1, std::memory_order_relaxed);
                // 份数在派发时就能确定：批次一旦入队必然整批执行，收件人集合不可变。
                crossThreadDeliveries_.fetch_add(g.conns->size(), std::memory_order_relaxed);
            }
        }
    }

    Options opt_;

    // 订阅 ID 分配器：registry 级全局单调，【不随房间回收而重置】。
    //
    // 原实现是 Room 的成员 nextId，每个房间从 0 重新开始 —— 于是
    // 「房间空掉被回收 → 同名房间重建」之后，ID 空间从头复用。
    // 而 unsubscribe 是按 (room 名, id) 定位的：若某个旧连接对应的
    // ID 1 在房间重生后与新连接的 ID 1 撞上，一次迟到的退订就会
    // 误删新订阅者（幂等注释在这种情况下不成立）。
    // 全局单调分配让「同一个 ID 在同一房间名下的历史里只出现一次」，
    // 这类跨代误删在结构上不可能发生。
    std::atomic<SubscriberID> nextSubId_{0};

    mutable std::shared_mutex roomsMtx_;
    phmap::flat_hash_map<std::string, std::shared_ptr<Room>> rooms_;

    // 已回收房间的序号水位线：房间空掉被摘除时把它的 seq 存下来，同名房间重建时续上。
    //
    // 为什么必须有：房间在最后一个订阅者离开时会被整体回收（内存友好），但
    // 「同名房间重建 ⇒ 序号从 1 重来」会让接收端的 (实例, 房间) 基线倒退，
    // 每次房间重建都误报一次大规模乱序。存水位线后序号在进程生命周期内单调。
    //
    // 容量：键是「历史上出现过的房间名」，正常业务下房间名是有限集合（站点/群 ID）。
    // 万一真的无界增长（房间名带随机后缀的恶意用法），到上限就整体清空 ——
    // 代价只是「之后重建的房间序号可能倒退一次」，而这只影响诊断计数，
    // 不影响任何消息投递语义。宁可这样，也不要让一个诊断设施变成内存泄漏。
    static constexpr size_t kMaxSeqWatermarks = 1 << 16;
    phmap::flat_hash_map<std::string, uint64_t> roomSeqWatermark_;

    std::mutex poolMtx_;
    std::condition_variable poolCv_;
    std::deque<std::shared_ptr<Shard>> ready_;
    // 无锁就绪计数：worker 自旋阶段据此判断「有没有活」，避免为了看一眼队列就抢锁
    std::atomic<size_t> readyCount_{0};
    std::atomic<size_t> parked_{0};
    bool stop_{false};
    std::vector<std::thread> workers_;

    std::atomic<size_t> dropped_{0};

    // 分组投递效果计数（扇出热路径上每个分组各一次，即 O(loop 数) 而非 O(订阅者数)）
    mutable std::atomic<size_t> inLoopDeliveries_{0};
    mutable std::atomic<size_t> crossThreadBatches_{0};
    mutable std::atomic<size_t> crossThreadDeliveries_{0};
    mutable std::atomic<size_t> fanoutExceptions_{0};

    // 全局「在途投递份数」：已派发到目标 loop 队列、尚未执行完的收件人份数之和。
    //
    // 用 shared_ptr<atomic> 而不是裸成员：投递 lambda 会在【目标 loop 的队列里】
    // 多活一会儿，可能活过 registry 本身（压测/退出时控制器先析构、队列里还有批次）。
    // lambda 捕获这个 shared_ptr 就自带生命周期，不会读到一个已析构的 atomic。
    // 该指针构造后不再改指向，只改 pointee，因此无并发读指针的问题。
    mutable std::shared_ptr<std::atomic<size_t>> inFlight_{
        std::make_shared<std::atomic<size_t>>(0)};
    // 因在途上限被拒的 publish 次数。与 dropped_ 的区别：
    // dropped_ 是「所有被拒之和」，这个是「因下游 loop 队列积压被拒」的那部分 ——
    // 两者的差额就是分片队列满（backlogPerShard）导致的拒绝。
    // 分开才能判断该调哪个阈值。
    mutable std::atomic<size_t> inflightRejected_{0};
};

// 生产实例：drogon WebSocket 连接 + trantor 事件循环句柄
using RoomRegistry = RoomRegistryT<drogon::WebSocketConnectionPtr, TrantorLoopHandle>;

#endif // LOONG_BOOT_ROOM_REGISTRY_H
