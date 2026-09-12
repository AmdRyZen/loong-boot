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
  public:
    using SubscriberID = uint64_t;
    using ConnPtr = ConnT;

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
        // 单次连续扇出的消息批上限，超过则让出 worker 重排队，避免热点分片饿死其他房间
        size_t drainBatch = 32;
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

            const SubscriberID id = ++r->nextId;
            const size_t after = r->subCount.load(std::memory_order_relaxed) + 1;
            const size_t want = shardsFor(after);

            if (want != r->shards.size())
            {
                // 分片数跨越阶梯：走安全重建协议（见 reshapeLocked 注释）
                reshapeLocked(*r, want, Entry{id, conn, loop});
            }
            else
            {
                const size_t idx = static_cast<size_t>(id % want);
                if (r->dist[idx].empty())
                {
                    r->nonEmptyShards.fetch_add(1, std::memory_order_relaxed);
                }
                r->dist[idx].push_back(Entry{id, conn, loop});
                r->idToShard[id] = idx;
                r->shards[idx] = refreshSnapshot(r->shards[idx], r->dist[idx]);
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
            return;
        }
        const size_t idx = it->second;
        auto& bucket = r->dist[idx];
        std::erase_if(bucket, [id](const Entry& e) { return e.id == id; });
        r->idToShard.erase(it);
        r->shards[idx] = refreshSnapshot(r->shards[idx], bucket);
        if (bucket.empty())
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
        }
    }

    /**
     * @brief 向房间发布一条消息（payload 已序列化完成）
     * @return false 表示分片积压达上限、消息被丢弃（调用方需计数并告知客户端）
     */
    bool publish(const std::string& room, const std::string& payload)
    {
        return publishShared(room, std::make_shared<const std::string>(payload));
    }

    bool publish(const std::string& room, std::string&& payload)
    {
        return publishShared(room, std::make_shared<const std::string>(std::move(payload)));
    }

    bool publishShared(const std::string& room, std::shared_ptr<const std::string> payload)
    {
        std::shared_ptr<Room> r;
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
        bool ok = true;
        {
            // 房间级发布锁：消息顺序的唯一来源。只做「入队」，不做扇出。
            std::lock_guard pubLock(r->pubMtx);
            for (size_t i = 0; i < r->dist.size(); ++i)
            {
                if (r->dist[i].empty())
                {
                    continue; // 空槽位不建对象也不发布，小房间不为用不到的分片买单
                }
                const std::shared_ptr<Shard>& sh = r->shards[i];
                std::lock_guard shardLock(sh->mtx);
                if (sh->queue.size() >= opt_.backlogPerShard)
                {
                    sh->dropped.fetch_add(1, std::memory_order_relaxed);
                    dropped_.fetch_add(1, std::memory_order_relaxed);
                    ok = false;
                    continue;
                }
                sh->queue.push_back(payload);
                // 门铃：仅在「空闲 → 忙」跳变时入池，热点分片不会每条消息都惊动线程池。
                // exchange 与 drain 的 busy=false 都在分片锁内完成，不会丢唤醒。
                if (!sh->busy.exchange(true, std::memory_order_acq_rel))
                {
                    needWake.push_back(sh);
                }
            }

            // 在 pubMtx 内入池（内联模式除外）：保证「busy == true ⟹ 分片已在池中或正在被抽」，
            // 让 reshapeLocked 的自旋等待一定有进展，不会等一个没人抽的分片。
            if (!inlineMode)
            {
                for (const auto& sh : needWake)
                {
                    enqueueReady(sh);
                }
            }
        }

        if (needWake.empty())
        {
            return ok; // 已有线程在抽这些分片，它会在循环里取到本条消息
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
        return ok;
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
        std::atomic<uint64_t> dropped{0};
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
        SubscriberID nextId{0};
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

    // 分片数变化时的安全重建协议。调用方必须持有 r.pubMtx。
    //
    // 核心约束：只有当【全体订阅者都收到了截止此刻的全部消息】时，重新分片才是安全的。
    // 否则会出现「某人已经收过、某人还没收」的错位 —— 那是重复投递的来源
    // （早期版本把各分片残留 payload 统一搬运再广播，实测必然产生重复）。
    //
    // 而「全体排空」在本函数里是免费可得的：调用方持 pubMtx，不会再有新消息入队；
    // 又因为 publishShared 在 pubMtx 内就把分片入池，所以 busy == true 一定意味着
    // 「该分片在池中或正在被抽」，自旋等待 busy == false 必然收敛，且收敛时队列已空。
    // 换言之：持 pubMtx 时 busy == false ⟺ 队列为空且无在途扇出。
    //
    // 注意分组投递（A1）之后 busy == false 的语义收紧为「已把全部消息【派发】到目标
    // loop」，而不是「已送达」。这不影响本函数的正确性：reshape 只在既有订阅者之间
    // 重新分配分片，不增删成员，而每个已派发批次的连接列表是快照里的不可变集合，
    // 所以即使批次还在目标 loop 队列里等待执行，它的收件人集合也不会因 reshape 而改变。
    void reshapeLocked(Room& r, size_t newK, const Entry& extra)
    {
        for (auto& sh : r.shards)
        {
            if (!sh)
            {
                continue;
            }
            while (sh->busy.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        }

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
        constexpr int kSpinRounds = 4000;

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
            fanOutToSnapshot(*snap, payload);

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

            if (g.loop.isCurrentThread())
            {
                const std::string_view view(*payload);
                for (const auto& c : *g.conns)
                {
                    c->send(view);
                }
                inLoopDeliveries_.fetch_add(g.conns->size(), std::memory_order_relaxed);
            }
            else
            {
                // 只捕获两个 shared_ptr（payload + 连接组），不复制连接列表本身。
                // 连接对象因此至少活到这个批次被执行完，之后才可能析构。
                g.loop.dispatch([payload, conns = g.conns] {
                    const std::string_view view(*payload);
                    for (const auto& c : *conns)
                    {
                        c->send(view);
                    }
                });
                crossThreadBatches_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    Options opt_;

    mutable std::shared_mutex roomsMtx_;
    phmap::flat_hash_map<std::string, std::shared_ptr<Room>> rooms_;

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
};

// 生产实例：drogon WebSocket 连接 + trantor 事件循环句柄
using RoomRegistry = RoomRegistryT<drogon::WebSocketConnectionPtr, TrantorLoopHandle>;

#endif // LOONG_BOOT_ROOM_REGISTRY_H
