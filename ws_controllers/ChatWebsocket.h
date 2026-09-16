#pragma once
#include <drogon/WebSocketController.h>
#include "kafka/KafkaManager.h"
#include <glaze/glaze.hpp>
#include <drogon/HttpAppFramework.h>
#include "utils/retry_utils.h"
#include "utils/RateLimitedLog.h"
#include "utils/SnowflakeId.h"
#include "utils/ChatPersistEnvelope.h"
#include "parallel_hashmap/phmap.h"
#include "RoomRegistry.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <format>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// getpid()：用于生成可诊断的实例 ID（见构造函数）
#include <unistd.h>

#include <drogon/nosql/RedisClient.h>
#include <chrono>

using namespace drogon;

class ChatWebsocket final : public WebSocketController<ChatWebsocket>
{
public:
    ChatWebsocket() : core_(std::make_shared<Core>(makeFanoutOptions()))
    {
        // 生成当前服务实例唯一的 Instance ID（防止集群跨机广播回环）。
        // 用「进程号 + 启动纳秒时间戳」而不是 std::random_device：
        // 同样唯一（同机 PID 不同、跨机时间戳不同），但不会像 random_device 那样
        // 在某些平台上阻塞或在熵不足时抛异常，出问题时也能直接从 ID 看出是哪个进程。
        core_->instanceId =
            std::format("inst_{}_{}", static_cast<long long>(::getpid()), Subscriber::nowNanos());

        // 先把开关读进来。kafkaPersistenceEnabled() 的初值是 fail-safe 的 false，
        // 若不在这里同步一次，启动后到第一次定时刷新之间会处于「配置说开、实际关」的状态。
        reloadSwitches();

        // 把端到端延迟直方图的数据出口接到 Prometheus registry 上。
        // 注册表本身不依赖指标代码（单测不链接 drogon / 指标单例），所以接线放在这里。
        installLatencySink(*core_);

        // 注册定时任务：空闲超时连接驱逐 + 房间指标采集 + 开关热更新。
        //
        // 回调按值捕获 core_（shared_ptr），而不是捕获 this：
        // 回调持有的引用会让 Core 活到本次回调结束，因此即使控制器已析构、
        // 或静态析构顺序与预期不同，也不会出现 use-after-free。
        // （原先捕获 this 且丢弃了 TimerId，退出阶段存在悬空访问窗口。）
        HttpAppFramework::instance().getLoop()->runEvery(5.0, [core = core_] {
            checkAndEvictIdleConnections(*core);
            publishRoomMetrics(*core);
            publishKafkaMetrics();
            // 去重表 TTL 清理（去重关闭时表为空，立即返回）
            sweepDedupTable(*core);
            // 开关热更新：改 config.json 不必重启进程（见 reloadSwitches 注释）
            reloadSwitches();
        });

        // 初始化 Redis 分布式集群网关总线
        initClusterBus();
    }

    void handleNewMessage(const WebSocketConnectionPtr&,
                          std::string&&,
                          const WebSocketMessageType&) override;
    void handleNewConnection(const HttpRequestPtr&,
                             const WebSocketConnectionPtr&) override;
    void handleConnectionClosed(const WebSocketConnectionPtr&) override;

    WS_PATH_LIST_BEGIN
    // 只注册精确路径 /chat。
    // 原先还挂了 WS_ADD_PATH_VIA_REGEX("/[^/]*", Get)：任何「单段路径」都会被当成
    // WebSocket 升级请求处理，既扩大了攻击面，又可能吞掉本该 404 的请求。
    // /chat 由上面的精确匹配覆盖，删掉正则不影响客户端。
    WS_PATH_ADD("/chat");
    WS_PATH_LIST_END

private:
    // ── 单连接状态（存在 WebSocketConnection 的 context 里，每条连接一份）──────
    struct Subscriber
    {
        std::string topic_;
        std::string userName_;
        RoomRegistry::SubscriberID id_{};

        // 本连接所属房间的句柄（订阅成功后取一次，终身复用）。
        //
        // 缓存它是为了让「发布」这条热路径完全不过全局 roomsMtx_：
        // 否则每条消息都要拿一次全局共享锁 + 按房间名做一次哈希查找，
        // 而全站所有房间共用这一把锁 —— 那是唯一的全局串行点。
        // 房间被回收后这个句柄会自动失效（retired），publish 回退查表，无需手动清理。
        RoomRegistry::RoomHandle room_;

        // 本连接所属的 IO loop（建连时从当前线程取，终身不变）。
        //
        // 房间扇出走的是 RoomRegistry 内部按 loop 分组的派发；私聊直投原先没有
        // 这一步，于是「同一昵称的 N 个多端会话」会付 N 次跨线程唤醒（queueInLoop），
        // 而且没有任何在途额度可言 —— 目标 loop 卡住时 send 只入队不落地，内存无界上涨。
        // 缓存 loop 之后，私聊也能按 loop 分组派发（唤醒 O(loop 数)）并在派发前
        // 计入全局在途额度（见 Core::directInFlight）。
        TrantorLoopHandle loop_;

        // 跨线程访问：IO 线程写入（收到任意帧即刷新），主循环定时任务读取（空闲驱逐）。
        // 原先是非原子的 time_point —— 跨线程读写属于 data race（形式上 UB），
        // 这里用原子量存 steady_clock 纳秒计数消除竞争。
        static int64_t nowNanos()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        void touch()
        {
            lastActiveNanos_.store(nowNanos(), std::memory_order_relaxed);
        }

        // 过载通知（503「消息未投递」）的按连接限流。
        //
        // 为什么必须限流：饱和时「丢弃」是 O(丢弃数) 的，而每条丢弃若都回一帧通知，
        // 丢弃路径的成本就和真实投递一样高 —— 实测 30s 回声饱和压测里 14.36M 次丢弃
        // 曾产生 14.36M 次额外 send（外加 1.6GB 逐条 WARN 日志）。
        // 限流后既不静默（客户端仍会被明确告知过载），也不再随丢弃数线性放大。
        //
        // CAS 语义：只有成功把时间戳推进的那一方返回 true，天然去重，无需额外锁。
        bool shouldSendOverloadNotice(int64_t minIntervalNanos = 1'000'000'000LL)
        {
            const int64_t now = nowNanos();
            int64_t last = lastOverloadNoticeNanos_.load(std::memory_order_relaxed);
            if (now - last < minIntervalNanos)
            {
                return false;
            }
            return lastOverloadNoticeNanos_.compare_exchange_strong(
                last, now, std::memory_order_relaxed);
        }

        std::atomic<int64_t> lastActiveNanos_{nowNanos()};
        std::atomic<int64_t> lastOverloadNoticeNanos_{0};
    };

    // 一个在线会话：连接 + 它的 Subscriber。
    //
    // 快照里同时带上 Subscriber 的 shared_ptr，是为了让空闲驱逐**完全不必**再去访问
    // WebSocketConnection 的 context —— drogon 的 hasContext()/getContextRef() 读的是
    // 非原子成员（WebSocketConnection.h 的 contextPtr_），从主循环定时器跨线程调用
    // 属于 data race（形式上 UB）。Subscriber 里的字段都是原子量，跨线程读是安全的。
    struct Session
    {
        WebSocketConnectionPtr conn;
        std::shared_ptr<Subscriber> sub;
    };

    // ── 共享状态 ─────────────────────────────────────────────────────────────
    // 用 shared_ptr 持有：定时器与启动回调按值捕获它，让这些状态的生命周期不再依赖
    // 「控制器一定比定时器活得久」这一脆弱假设（drogon 的静态析构顺序并无保证）。
    struct Core
    {
        explicit Core(RoomRegistry::Options opt) : roomRegistry(std::move(opt))
        {
            // 私聊直投的在途额度上限，可用 LOONG_WS_MAX_DIRECT_INFLIGHT 覆盖（0 = 关闭限制）。
            size_t v = kDefaultMaxDirectInFlight;
            if (readEnvSize("LOONG_WS_MAX_DIRECT_INFLIGHT", v))
            {
                maxDirectInFlight = v;
            }
        }

        // ── 私聊直投的全局在途投递额度 ─────────────────────────────────────────
        //
        // 为什么需要：deliverDirectLocally 原先对每个目标会话裸调 conn->send()，
        // 没有任何上限。send 在跨线程时只是 loop->queueInLoop(lambda) —— 目标 loop
        // 一旦卡住（慢消费者 / 单核被打满），已入队的帧只增不减，内存无界上涨，
        // 而调用方拿不到任何背压信号（与 RoomRegistry 里 maxInFlightDeliveries
        // 修掉的那个问题同源，只是发生在私聊路径上）。
        //
        // 语义与房间扇出对齐：额度在【派发前】一次性扣减（全有或全无），
        // 在目标 loop 执行完这批 send 之后归还。因此它度量的是
        // 「已派发进 loop 队列、尚未执行的份数」，而不是「已送达」。
        //
        // 默认值与房间侧一致（2^18）：只是深度异常时的安全阀，正常负载碰不到。
        static constexpr size_t kDefaultMaxDirectInFlight = 1 << 18;
        size_t maxDirectInFlight = kDefaultMaxDirectInFlight;

        // 用 shared_ptr<atomic> 而不是裸成员：投递 lambda 会在目标 loop 的队列里
        // 多活一会儿，可能活过 Core 本身（退出时控制器先析构、队列里还有批次）。
        // 与 RoomRegistry::inFlight_ 同一手法。指针构造后不再改指向。
        std::shared_ptr<std::atomic<size_t>> directInFlight{
            std::make_shared<std::atomic<size_t>>(0)};

        // 房间注册表：按订阅者分片 + 多核并行扇出，严格保序。
        // 取代了原先的 drogon::PubSubService（它单线程扇出，房间越大越慢）
        // 与 RoomSerialDispatcher（保序职责已内聚到 RoomRegistry 的房间级发布锁）。
        RoomRegistry roomRegistry;

        // 上一次「最深积压房间」日志里写过的房间名。
        //
        // 放在 Core 里而不是做成 publishRoomMetrics 的函数内静态量：函数内静态量
        // 会让「同一个进程里两个控制器实例」共用一份状态（多实例测试时会互相清掉
        // 对方的去重标记），而且它的生命周期与静态析构顺序绑定，退出阶段是个隐患。
        std::string lastWorstRoomLogged;

        // ── 消息去重（客户端重发幂等）──────────────────────────────────────────
        //
        // 为什么需要：客户端在「没收到回显」时会重发（网络闪断后重连再发、
        // 用户连点、中间代理重试）。服务端若不识别，房间里的其他人会看到两条。
        //
        // 键 = (发送者昵称, 客户端 dto.key)。为什么必须带上发送者：key 由客户端
        // 提供，两个客户端完全可能撞上同一个值。
        //
        // ⚠️ 开关缺省 false，理由不是性能而是【协议前提】：
        //    本工程此前的 chat.html 把 key 填成【房间名】—— 那种客户端一旦开着去重，
        //    该用户在同一房间里的第二条消息起会被全部当成重发而静默丢弃。
        //    所以：服务端默认关；客户端必须为每条消息生成唯一 id
        //    （chat.html 已改成 crypto.randomUUID()）。两者缺一不可。
        struct DedupEntry
        {
            uint64_t msgId = 0; // 首次投递成功时分配的消息 id（重发时用它回显）
            int64_t atMs = 0;   // 首次投递成功时刻（TTL 清理用）
            // 首次投递成功时该消息的房间序号。重发的 ACK 要带上它，
            // 否则客户端会把重发当成「seq 缺失」而在下一次 sync 里重复拉取。
            uint64_t seq = 0;
        };
        phmap::flat_hash_map<std::string, DedupEntry> dedupSeen;
        std::mutex dedupMtx;

        // 去重表容量上限：到上限整体清空（与 roomSeqWatermark_、集群序号基线
        // 同一手法）。代价是清空后的一小段窗口里重发不再被识别，
        // 而绝不因此让一个「优化」设施变成内存泄漏。
        static constexpr size_t kMaxDedupEntries = 1 << 16;
        // 去重窗口。超过就清掉 —— 除了回收内存，更重要的是限制
        // 「客户端复用同一个 key」这种错误用法的杀伤范围：没有 TTL 的话，
        // 一个 key 填成房间名的客户端会从第二条消息起永久被静默丢弃。
        static constexpr int64_t kDedupTtlMs = 120'000;

        // 用户名 -> 该用户名下的所有在线会话（同一昵称允许多端并存，多端都能收到私聊）
        // 原先的 name -> 单连接 语义在「同名多连接」时会串号：旧连接断开时会把新连接的
        // 记录一并 erase 掉，导致新用户从此收不到私聊。
        phmap::parallel_flat_hash_map<std::string, std::vector<Session>> userNameToConn;

        // 保护 userNameToConn 的复合操作与遍历。
        // 读路径（私聊查表 / 定时任务遍历）走共享锁，连接注册与注销走独占锁，
        // 彻底消除原实现「无锁遍历 vs IO 线程 emplace/erase」的数据竞争。
        mutable std::shared_mutex connMutex;

        // 在线连接总数：只用于给驱逐快照的 reserve 做估算。
        //（原先按「用户数」预留，同名多端时会严重低估，导致每次快照都重新分配。）
        std::atomic<size_t> connCount{0};

        std::string instanceId;
        std::shared_ptr<drogon::nosql::RedisSubscriber> clusterSubscriber;

        // 集群入站包的房间序基线：key = 发送实例ID + '\x1f' + 房间名 → 已见最大序号。
        // 只用于诊断（缺口 / 乱序检测），不参与任何投递决策 —— 见 ClusterPacket::roomSeq。
        std::mutex clusterSeqMtx;
        phmap::flat_hash_map<std::string, uint64_t> clusterLastSeq;

        // 集群总线【实际可用】标志：订阅注册成功后才置 true。
        //
        // 为什么不能只看配置开关 clusterBusEnabled()：配置说 true 但
        // Redis 客户端缺失 / newSubscriber 失败 / subscribe 抛异常时，
        // 总线实际上是死的。此时若仍按「总线开着，在线状态不可知」处理，
        // 私聊会跳过 404 直接回显 200 —— 发送者收到「成功」而消息其实
        // 哪儿都没去（假 ACK）。用这个运行时标志替代，才能诚实回答
        // 「本实例现在到底能不能跨实例投递」。
        std::atomic<bool> clusterBusReady{false};
    };

    std::shared_ptr<Core> core_;

    void initClusterBus();
    // 把一条消息广播给集群其他实例。
    //
    // toUser 为空 → 房间广播（接收方按 topic 投给本地房间订阅者）
    // toUser 非空 → 私聊（接收方按 toUser 投给本地同名会话）
    //
    // 两种模式共用同一条 Redis 频道、同一个 instId 回环过滤、同一个 JSON 结构，
    // 只靠 toUser 空/非空区分 —— 少一套解析与订阅生命周期，代价是私聊也要过一次
    // 房间广播那条路（详见 publishToCluster 实现里的取舍说明）。
    //
    // roomSeq：房间广播时带上 RoomRegistry 分配的房间级序号（0 = 未分配）。
    // 接收端据此做缺口/乱序检测，见 ClusterPacket::roomSeq。
    static void publishToCluster(const Core& core, const std::string& topic,
                                 const std::string& json, const std::string& toUser = {},
                                 uint64_t roomSeq = 0);

    // 用入站包的房间序号更新基线，并检测「缺口」与「回退（乱序）」。
    //
    // 只做检测与计数，【不】改变投递行为：
    //   · 缺口（seq 跳跃）→ ws_cluster_seq_gap_total += 跳跃掉的条数
    //   · 回退（seq <= 已见最大值）→ ws_cluster_out_of_order_total += 1
    // 两者都会触发限流告警（触发频率与丢包/乱序量成正比，不能逐条打）。
    //
    // 不做回拉补齐：补齐需要一个「可按 (房间, 序号) 区间回放」的持久化日志
    //（Redis Stream / Kafka 带 partition key），那是另一套设施。当前阶段的目标
    // 是把「跨实例到底有没有丢/乱序」从「无从判断」变成「有数可查」。
    static void checkClusterRoomSeq(Core& core, const std::string& originInstId,
                                    const std::string& room, uint64_t roomSeq);

    // 把一条已序列化的私聊消息投递给【本实例上】该用户名的所有在线会话。
    // 返回投递到的会话数（0 = 本实例上没有该用户名的在线会话）。
    //
    // sender 用来判断「发送者本人是否也在目标会话列表里」（同昵称多端）：
    // 在，说明他已经收到自己那一份，调用方不必再补一帧回显。
    //
    // backpressured 非空时写回「因在途额度不足而整批未投」的会话份数：
    //   0  = 未被背压拒绝（0 份投递 + 0 份背压 = 本实例确实没有这个昵称）
    //   >0 = 本实例有这个昵称，但在途额度已满 ⇒ 调用方应回 503 而不是 404/200
    // 两者的区别至关重要：404 会让客户端以为「人不在线」，503 才是「服务器忙」。
    //
    // ⚠️ 本函数会被两个线程调用：① 发送方的 IO 线程（本地投递）
    //    ② Redis 订阅回调线程（跨实例收到的私聊）。
    //    因此内部【不得】读目标连接的非原子成员（connected()/hasContext()）——
    //    目标连接属于另一个 IO 线程，跨线程读它们是 data race（形式上 UB）。
    //    已断开连接上调用 send 是安全的（drogon 静默丢弃），且条目会在
    //    handleConnectionClosed 里摘除，所以「表里有」≈「在线」。
    static size_t deliverDirectLocally(Core& core,
                                       const std::string& targetUser,
                                       const std::string& json,
                                       const WebSocketConnectionPtr& sender,
                                       bool* echoedToSender = nullptr,
                                       size_t* backpressured = nullptr);
    // 形参用 string_view：开关关闭时（压测/开发常态）调用方不必先构造 std::string，
    // 避免「已经拷贝完了才在函数里 short-circuit」这种白付的分配。
    // 需要所有权的地方（TBB worker）在 lambda 捕获里再落成 std::string。
    static void produceKafkaAsync(std::string_view topicName, std::string_view payload,
                                  std::string_view key = {});

    // 注：这里曾经用 std::pmr::string，但那是个半成品 —— 全工程从未创建过任何
    // memory_resource，pmr 容器默认落到 new_delete_resource()，即与 std::string
    // 同一条分配路径，却额外多带一个 allocator 指针（每字段 +8B，4 字段的 DTO
    // 直接胖一圈）并多一层虚调用。比 std::string 更慢更大，故退回 std::string。
    // 若将来真要做端到端 pmr，正确做法是在 IO 线程上挂
    // thread_local std::pmr::monotonic_buffer_resource 并在构造 VO 时显式传 &res，
    // 而不是只把字段类型换掉。
    struct chatMessageDto
    {
        std::string key;
        std::string action;
        std::string msgContent;
        std::string toUser;  // 点对点私聊目标用户名 (为空表示房间广播)

        // ── B1：客户端请求标识 ────────────────────────────────────────────────
        //
        // 客户端为每条【待确认】的消息生成一个唯一值，服务端在 ACK 里原样回传，
        // 客户端据此把「这条 ACK 对应哪条本地消息」对上号。
        //
        // 与 key 的区别：key 是【幂等键】（同一条消息重发时保持不变，用于去重）；
        // requestId 是【这次投递尝试的标识】。两者可以相同，语义不同 ——
        // key 决定「要不要重复投递」，requestId 决定「这个 ACK 是回给哪一次的」。
        //
        // 空 = 老客户端，服务端不发 ACK（保持既有行为）。
        std::string requestId;

        // ── B4：重连补齐游标 ──────────────────────────────────────────────────
        //
        // 仅 action == "sync" 时使用：客户端已见到的最大房间序号。
        // 0 = 没有游标（服务端不回放，见 RoomRegistry::replay 的说明）。
        uint64_t sinceSeq = 0;
    };

    // 客户端可见的消息 VO。刻意保持最小：落库/回放需要的上下文在下面的
    // chatPersistVo 里，不要往这里加字段（那会让每条消息的线格式都变胖）。
    //
    // 唯一的例外是 type —— 见下。
    struct chatMessageVo
    {
        int code = -1;
        uint64_t id = 0;
        std::string name;
        std::string message;

        // "message" = 真人发言（房间广播 / 私聊）；"notice" = 系统提示与入群/退群公告。
        //
        // 为什么允许它进线格式（而房间名 / 目标昵称 / 实例 ID 不行）：
        // ① 它是【客户端真的需要】的信息，不是只有回放才用 —— 公告的 code 也是 200、
        //    name 也是房间名，客户端此前只能靠「房间名相等 + 正则匹配文案」去猜，
        //    改一句公告文案就失效（chat.html 里的老实现正是如此）。
        // ② 它很短（4~7 字节的枚举字面量），不像房间名/实例 ID 是长字符串，
        //    不会把每条消息的线格式撑胖。
        //
        // 加字段对老客户端是安全的：JSON 多一个键会被忽略。
        std::string type;

        // ── B3/B4：房间级序号 ─────────────────────────────────────────────────
        //
        // 本条消息在该房间内的单调序号（见 RoomRegistry::publishShared 的分配点）。
        // 0 = 不适用（私聊 / 未分配 / 被背拒）。
        //
        // 为什么允许它进线格式：它是【客户端做重连补齐的必要条件】——
        // 客户端得先知道自己看到哪儿了，才能在重连时把游标报回来（action:"sync"）。
        // 顺带也让客户端能做「按 seq 去重」与「发现缺口」，比按 id 去重更准
        //（id 是全局 snowflake，不能用来判断房间内是否漏了一条）。
        //
        // ⚠️ 它是【每实例】单调的，跨实例不可比较（见 ClusterPacket::roomSeq）。
        //    客户端重连到另一个实例时，游标空间对不上 —— 服务端会在 sync_done
        //    里用 code 206 明确告知「本次不是完整补齐」。
        uint64_t seq = 0;

        // ── B1：ACK 回执 ──────────────────────────────────────────────────────
        //
        // type == "ack" 时才有意义：原样回传客户端请求里的 requestId，
        // 让客户端把回执与本地那条待确认消息对上号。
        std::string requestId;
    };

    // ── Kafka 落库信封 ──────────────────────────────────────────────────────
    //
    // 为什么不直接落客户端 VO（chatMessageVo）：客户端 VO 只带「显示这条消息
    // 所需的最少字段」（code/id/name/message）。落库与回放需要的是【上下文】：
    // 这条消息属于哪个房间、是不是私聊、发给谁、房间内第几条、哪个实例写的。
    // 原实现落的就是客户端 VO，于是回放端拿到一堆「name + message」，无法还原
    // 它们属于哪个房间 —— 历史回放实际上做不了（本项缺陷的根因）。
    //
    // 刻意不往 chatMessageVo 上加字段：那会把「只有回放需要」的字段发给每一个
    // 客户端，每条消息的线格式都变胖（房间名 / 目标昵称 / 实例 ID 都是长字符串）。
    // 两条路分开：客户端看到的最小 VO 不变，落库的是自描述信封。
    //
    // （chatMessageVo 上的 type 是唯一例外 —— 它短、且客户端真的需要，
    //   理由见那个字段自己的注释。这里的 room/toUser/originInstance 仍然只落库。）
    // ⚠️ 结构与序列化已抽到 utils/ChatPersistEnvelope.h（namespace loong::chat）。
    //
    // 为什么抽出去：这几个东西原本是这里的 private static，只有「跑真实例 + 连真 broker
    // + 消费主题」才能验证（e2e 级），而它们恰恰是最容易写错又最难发现的一类逻辑 ——
    // 信封少一个键 ⇒ 回放端按固定 schema 读会静默缺字段；私聊分区键忘了排序 ⇒
    // A→B 与 B→A 落不同分区、会话内顺序静默丢失。抽成纯函数后普通单测就能锁死
    // （test/test_chat_persist.cc）。
    //
    // 这里保留同名类型别名，让下面 persistXxx 的代码尽量少改。
    // （自由函数不能用类作用域 using 声明引入 —— 那是给基类成员用的 —— 所以
    //   buildPersistJson / nowMs 在调用处写全限定名。）
    using chatPersistVo = loong::chat::PersistVo;

    // 落库一条房间消息 / 公告。
    //
    // 分区键用【房间名】：相同 key 落同一分区 ⇒ 分区内保序 ⇒ 回放端按分区顺序
    // 读即可还原房间内的相对顺序。原实现不传 key，librdkafka 会轮询分区，同一个
    // 房间的消息散落到不同分区 —— 这是「Kafka 历史回放路径本身不保序」的直接原因。
    static void persistRoomMessage(const Core& core, const std::string& room,
                                   const std::string& sender, const chatMessageVo& vo,
                                   uint64_t roomSeq, const char* type,
                                   const std::string& clientMsgId = {})
    {
        // ⚠️ 开关检查必须放在【构造信封之前】，而不是只放在 produceKafkaAsync 里。
        //
        // produceKafkaAsync 的形参已经是 string_view（调用它本身零拷贝），但它收到的
        // 那个 payload 是【调用方提前构造好的】：下面的字段赋值会做 7~8 次 std::string
        // 拷贝，buildPersistJson 还要跑一遍完整 glaze 序列化并新分配一个字符串。
        // Kafka 关闭（缺省配置）时这些全部白付。
        //
        // 实测（clang -O2 + 本工程 glaze）：信封填充 + glz::write_json = 222.8 ns/条，
        // 同尺寸纯字符串拼接只要 47.4 ns；作为对照，发布热路径里 publish 调用本身
        // 约 115 ns/次 —— 这份白付比它旁边真正的投递工作还贵一倍。
        // 房间消息与私聊每条都走这里，直接落在吞吐上。
        if (!kafkaPersistenceEnabled().load(std::memory_order_relaxed))
        {
            return;
        }

        chatPersistVo p{};
        p.id = vo.id;
        p.name = vo.name;
        p.message = vo.message;
        p.room = room;
        p.type = type;
        p.roomSeq = roomSeq;
        p.sender = sender;
        p.clientMsgId = clientMsgId;
        p.originInstance = core.instanceId;
        p.ts = loong::chat::nowMs();
        produceKafkaAsync("chat_messages_topic", loong::chat::buildPersistJson(p), room);
    }

    // 落库一条私聊。
    //
    // 分区键用【排序后拼接的双方昵称】而不是发送者或接收者：同一个会话的两个
    // 方向（A→B 与 B→A）必须落同一个分区，否则回放时两个方向散在不同分区，
    // 会话内的先后顺序就丢了。
    static void persistDirectMessage(const Core& core, const std::string& sender,
                                     const std::string& targetUser, const chatMessageVo& vo,
                                     const std::string& clientMsgId = {})
    {
        // 同 persistRoomMessage：开关检查必须在构造信封之前，否则默认配置下
        // 每条私聊都白付一次结构体填充 + JSON 序列化（实测 222.8 ns/条）。
        if (!kafkaPersistenceEnabled().load(std::memory_order_relaxed))
        {
            return;
        }

        chatPersistVo p{};
        p.id = vo.id;
        p.name = vo.name;
        p.message = vo.message;
        p.toUser = targetUser;
        p.type = "direct";
        p.sender = sender;
        p.clientMsgId = clientMsgId;
        p.originInstance = core.instanceId;
        p.ts = loong::chat::nowMs();

        // 排序拼接的逻辑在 loong::chat::directPartitionKey（有单测锁住「两方向同键」）。
        produceKafkaAsync("chat_direct_topic", loong::chat::buildPersistJson(p),
                          loong::chat::directPartitionKey(sender, targetUser));
    }

    // Kafka 落库开关的缓存值（缺省 false = fail-safe，见 .cc 里的说明）。
    //
    // 用原子量而不是 `static const`：后者只在首次调用时求值一次，
    // 于是「改 config.json 不生效、必须重启」—— 压测时切换落库开关很烦。
    static std::atomic<bool>& kafkaPersistenceEnabled() noexcept;

    // 消息去重（客户端重发幂等）开关：读 custom_config.enable_message_dedup，
    // 缺省 false。理由见 Core::dedupSeen 上方的注释 —— 默认开会让「key 填成房间名」
    // 的既有客户端静默丢消息，方向性的错误不能靠默认值兜。
    static std::atomic<bool>& messageDedupEnabled() noexcept;

    // ── 去重的两阶段接口 ──────────────────────────────────────────────────────
    //
    // 早期实现把「判重」和「登记」合成一步（查到没有就立刻插入），于是：
    //   首投被背压拒绝（503，既没广播也没落库）→ 客户端按约定重发
    //   → 重发查到了那条【从未投递成功】的登记 → 判为重发 → 静默吞掉 + 回 200。
    // 结果是消息永久丢失，而客户端收到的是「成功」。登记必须与「投递被接受」对齐。
    //
    // 现在拆开：lookupDedupEntry 只查不登记（判重时调用）；registerDedupEntry 在
    // 投递真的被接受之后调用（房间：fanOutRoom 返回 true；私聊：过了 503 与 404 两道闸）。
    //
    // 取舍：这样放弃了「同一 key 并发重入」那一小段窗口的原子性 —— 两个同 key 的
    // 请求可能都通过 lookup 而各投一次（at-least-once）。这是刻意选的失败方向：
    // 重复投递对使用者可见、可容忍；静默丢失不可见、不可恢复。
    // 何况同一连接的消息在同一个 IO loop 上串行处理，同一客户端无法与自己竞争。
    // 返回首次投递成功时分配的消息 id（0 = 没查到）。outSeq 非空时写回它的房间序号。
    static uint64_t lookupDedupEntry(Core& core, const std::string& key, uint64_t* outSeq = nullptr);
    static void registerDedupEntry(Core& core, const std::string& key, uint64_t msgId, uint64_t seq);

    // 重读配置里的各个运行期开关。由构造函数与 5 秒定时任务调用。
    // 只覆盖【可以安全热更新】的开关；enable_cluster_bus 不在此列（见下）。
    static void reloadSwitches();

    // 集群总线开关：读 custom_config.enable_cluster_bus，缺省为 false。
    //
    // ⚠️ 未启用时【绝不能】调用 app().getRedisClient() / app().getFastRedisClient()。
    // drogon 的 RedisClientManager::getRedisClient(name) 内部是
    //     assert(map.find(name) != map.end());
    //     return map[name];                 // ← operator[]
    // 名字不存在时 operator[] 会往 redisClientsMap_ 里插入一个【空的 shared_ptr】，
    // Release 构建下 assert 被裁掉，于是静默留下一个空条目；
    // 进程退出时 ~RedisClientManager() 会对它做虚调用 closeAll()
    // （`for (auto& p : redisClientsMap_) p.second->closeAll();`）
    // → 从地址 0 取 vtable → SIGSEGV at 0x0，实测退出码 139。
    // 该崩溃发生在 drogon 的 quit() 里，应用侧 try/catch 拦不住，只能不触发它。
    //
    // 另注：本工程 redis_clients 配的是 is_fast=true，所以集群总线也必须用
    // getFastRedisClient()；取非 fast 变体同样会踩上面这个空条目。
    //
    // ⚠️ 本开关刻意【不】参与热更新（与 kafkaPersistenceEnabled 的区别）：
    // 它在构造期一次性决定要不要注册 Redis 订阅，注册之后无法中途注销 ——
    // 热更新只会造出「开关读作 false、总线却还活着」这种自相矛盾的状态。
    // 要改它必须重启。默认值与 Kafka 开关对齐（都是 false = fail-safe）。
    static bool clusterBusEnabled();

    struct ClusterPacket
    {
        std::string instId;
        std::string topic;
        std::string json;
        // 私聊目标用户名：为空 = 房间广播（按 topic 投给本地房间），
        // 非空 = 私聊（投给本实例上该用户名的所有在线会话）。
        //
        // 加这个字段是向后兼容的：glaze 对 JSON 里缺失的键不报错、成员保持默认值
        //（已实测 error_code=0），所以旧版本实例发来的包仍能正常解析。
        // 反向（新发旧收）会让旧实例遇到未知键而丢弃该包 —— 只在滚动升级窗口内出现。
        std::string toUser;

        // 发送方为该房间分配的单调序号（见 RoomRegistry::publishShared 的说明）。
        // 0 = 未分配（无订阅者 / 被背压拒绝 / 私聊包）。
        //
        // 接收端按 (instId, topic) 记 lastSeq，据此发现「总线链路上丢了消息」与
        // 「同一来源的消息被乱序投递」。检测到即计数 + 限流告警，不做回拉补齐
        //（补齐需要一个持久化的、可按 (房间, 序号) 区间回放的日志，属另一套设施）。
        uint64_t roomSeq = 0;
    };

    // 集群入站包的序号基线：key = 发送实例 ID + '\x1f' + 房间名。
    //
    // 用 phmap::flat_hash_map + 一把互斥量而不是加锁分片：入站处理本身已经是
    // 「每条跨实例消息一次」的频率，且订阅回调只有一个线程，锁竞争可忽略。
    // 用互斥量而不是「回调单线程所以免锁」的假设：drogon 的 RedisSubscriber
    // 回调线程归属是实现细节，不该让正确性依赖它。
    //
    // ⚠️ 会随「发送实例重启」而积累过期键（instId 含 pid+纳秒，重启即变）。
    //    到上限整体清空 —— 代价只是「清空后每个来源要重新建立一次基线」，
    //    期间不做缺口判断（宁可不报，也不误报）。绝不因此误报缺口。
    static constexpr size_t kMaxClusterSeqBaselines = 1 << 12;

    // 读一个「无符号整数」环境变量。返回 false 表示未设置或非法（out 保持不变）。
    // 只解析不抛：启动期不该因为一个环境变量写错就崩。
    static bool readEnvSize(const char* key, size_t& out)
    {
        if (const char* v = std::getenv(key); v != nullptr && *v != '\0')
        {
            try
            {
                out = static_cast<size_t>(std::stoull(v));
                return true;
            }
            catch (...)
            {
                // 非法值静默忽略，沿用默认值
            }
        }
        return false;
    }

    // 扇出参数：默认值适配本机，可用环境变量覆盖（便于压测 A/B 调参，不依赖配置加载时序）
    static RoomRegistry::Options makeFanoutOptions()
    {
        RoomRegistry::Options opt;
        // 默认不启用扇出线程池（opt.fanoutThreads 保持 0 = 单分片内联扇出）。
        // 多分片并行扇出是实验特性：保序正确性已由单测覆盖，但回声饱和压测下
        // 吞吐不稳定且低于内联路径，需要时用 LOONG_WS_FANOUT_THREADS 显式打开。
        readEnvSize("LOONG_WS_MAX_SHARDS", opt.maxShards);
        readEnvSize("LOONG_WS_SUBS_PER_SHARD", opt.subsPerShard);
        readEnvSize("LOONG_WS_FANOUT_THREADS", opt.fanoutThreads);
        readEnvSize("LOONG_WS_INLINE_MAX_SUBS", opt.inlineMaxSubs);
        readEnvSize("LOONG_WS_BACKLOG_PER_SHARD", opt.backlogPerShard);
        readEnvSize("LOONG_WS_DRAIN_BATCH", opt.drainBatch);
        readEnvSize("LOONG_WS_MAX_INFLIGHT", opt.maxInFlightDeliveries);
        readEnvSize("LOONG_WS_WORKER_SPIN_ROUNDS", opt.workerSpinRounds);
        // 房间消息日志容量（回放用；0 = 关闭回放）。
        readEnvSize("LOONG_WS_JOURNAL_CAP", opt.journalCap);
        // 端到端延迟直方图的采样掩码（必须是 2^n - 1；0 = 关闭采样）。
        // 用局部 size_t 中转：readEnvSize 的形参是 size_t&，而这里刻意用
        // uint64_t 存掩码，直接传引用在 LP64 上能编过、但换平台就是隐患。
        size_t sampleMask = static_cast<size_t>(opt.latencySampleMask);
        if (readEnvSize("LOONG_WS_LATENCY_SAMPLE", sampleMask))
        {
            opt.latencySampleMask = sampleMask;
        }
        return opt;
    }

    // 下面两个都由 5 秒定时任务驱动，因此做成静态函数 + 显式传入 Core：
    // 定时器回调捕获的是 core_（shared_ptr），不依赖控制器的生命周期。
    static void checkAndEvictIdleConnections(Core& core);

    // 把 RoomRegistry 的房间侧快照与扇出分组计数推送到 Prometheus registry。
    // 由 5 秒定时任务驱动：/metrics 抓取时就不必再去加房间表的锁，
    // 代价是最多 5 秒的滞后（gauge 类指标可以接受；counter 看增量也不受影响）。
    static void publishRoomMetrics(Core& core);

    // ── B4：重连补齐 ──────────────────────────────────────────────────────────
    //
    // 处理 action == "sync"：按客户端游标从房间日志回放缺口，再回一条 sync_done
    // 把房间当前的最大序号告诉它。语义边界见 RoomRegistry::replay。
    static void handleSync(Core& core, const WebSocketConnectionPtr& wsConn, const chatMessageDto& dto,
                           const std::string& room);

    // ── B1：逐条 ACK ──────────────────────────────────────────────────────────
    //
    // 把「这条消息到底投出去了没有」变成一个显式、可对号入座的回执，
    // 取代原先「回显即成功」的隐式约定（回显只说明服务端看到了这条消息，
    // 不说明它被广播出去了 —— 背压丢弃时两者一致，因为那时只回 notice）。
    //
    // requestId 为空 ⇒ 什么都不发：老客户端拿到的行为与改动前逐字节相同。
    // 失败原因写进 message 字段，客户端可以直接展示，不必再解析 code 猜文案。
    static void sendAck(const WebSocketConnectionPtr& wsConn, const std::string& requestId, int code,
                        uint64_t id, uint64_t seq, std::string_view reason);

    // 把 KafkaManager 的投递失败 / 日志抑制计数拉进 Prometheus registry。
    //
    // 为什么要「拉」而不是在失败点直接写指标：投递报告回调运行在 librdkafka 的
    // poll 线程里（且被 rd_kafka_poll 串行化），在那种地方做任何额外工作都会
    // 直接拖慢 poll；而 kafka-core 是独立编译的静态库，不该依赖上层的指标单例。
    // 每 5 秒拉一次既解耦又零热路径成本，代价是最多 5 秒滞后（counter 看增量无碍）。
    static void publishKafkaMetrics();

    // 把 RoomRegistry 的延迟采样出口接到 Prometheus registry。
    //
    // 为什么单独一个函数（而不是在构造函数里直接写 lambda）：那需要在本头文件里
    // include utils/PrometheusMetrics.h，而后者又 include TbbCoroutinePool.h ——
    // 一个 WebSocket 控制器头文件不该把 TBB 也拖进来。定义放在 .cc 里，
    // 头文件只留一个声明。
    static void installLatencySink(Core& core);

    // 清理去重表里过期的条目（TTL = Core::kDedupTtlMs）。由 5 秒定时任务驱动。
    // 去重关闭时表恒为空，本函数立即返回。
    static void sweepDedupTable(Core& core);

    // 全局唯一消息序号。
    //
    // 原实现是 `static std::atomic<uint64_t> seq{0}; fetch_add(1)+1` —— 纯进程内
    // 自增：① 重启后从 1 重来，与历史消息 id 冲突；② 多实例部署时各实例 id 空间
    // 完全重叠。两种情况下客户端都无法用 id 去重/排序，Kafka 回放里也分不清
    // 「同一条」和「两条内容相同」。
    //
    // 现改为 snowflake 式（41bit 毫秒 + 10bit 实例槽位 + 12bit 序号），
    // 跨重启、跨实例都不重复，且单实例内严格单调。实现见 utils/SnowflakeId.h。
    static uint64_t nextMessageId() noexcept
    {
        return loong::util::SnowflakeId::next();
    }

    // ── 客户端可控频率的日志限流 ────────────────────────────────────────────
    //
    // 下面两类日志的触发频率**由客户端决定**，逐条输出时一个恶意/异常客户端
    // 就能刷爆磁盘（与「过载时回一帧 503」是同一类放大漏洞）：
    //   ① 房间积压丢弃：客户端发得越猛，丢弃越多。实测 30s 回声饱和压测
    //      曾写出 1.6GB / 13.8M 行日志，日志 I/O 反过来拖垮扇出。
    //      三个调用点共用 overloadLogLimiter_（聊天消息 / 入群公告 / 退群公告）——
    //      它们同属「房间积压」这一种过载现象，共用一把尺子即可。
    //   ② JSON 解析失败：狂发非法负载即可无限刷。
    //
    // 它们都是真错误、不能静默，但必须限流。被压掉的次数会在下一次输出里
    // 一并带上（见 RateLimiter::takeSuppressed），信息不丢。
    // 判读总量仍看 counter：ws_messages_dropped_total / ws_json_parse_errors_total。
    loong::log::RateLimiter overloadLogLimiter_{1000};
    loong::log::RateLimiter jsonParseErrorLogLimiter_{1000};

    // 把 room 内的消息投递到本地房间（+ 集群总线 + Kafka 持久化，后两者按需开启）
    // 返回 false 表示「任一分片积压达上限 → 整条消息未入队」（调用方需计数并告知客户端）
    //
    // broadcastToCluster：是否把这条消息同步广播给集群其他实例。
    //   聊天消息 → true；入群/退群公告 → false。
    //   公告只描述「本实例上某个连接的状态变化」，而其他实例上可能恰有同名用户在
    //   同一房间 —— 广播过去会让对方收到「XX 已离开」这种与自己无关的误导信息。
    //
    // hint：发布者自己缓存的房间句柄（Subscriber::room_）。传进来即可跳过全局
    //   房间表锁；为空或不匹配房间名时 publish 会自动回退查表，因此传错不会崩，
    //   但会投错房间 —— 调用方必须保证 hint 与 topic 是同一个订阅者的成对字段。
    // build(序号) 返回本条的线格式负载。序号【必须】由 build 写进负载 ——
    // 客户端拿它做重连游标（见 chatMessageVo::seq），而它只能在注册表确定
    // 「这条消息会被接受」之后才分配，所以「构造负载」必须挪进注册表的临界区。
    // 无订阅者时 build 不会被调用（默认配置下不白付序列化开销）。
    template <typename BuildFn>
    bool fanOutRoom(Core& core, const std::string& topic, BuildFn&& build,
                    bool broadcastToCluster = true,
                    const RoomRegistry::RoomHandle& hint = {},
                    uint64_t* outRoomSeq = nullptr)
    {
        // 房间级序号由 RoomRegistry 在 pubMtx 内分配（见 publishShared 的说明）：
        // 拿到它才能让接收端判断「总线链路上有没有丢/乱序」，
        // 也才能把它写进 Kafka 落库信封供回放端还原顺序。
        uint64_t roomSeq = 0;
        std::shared_ptr<const std::string> built;
        const bool ok = core.roomRegistry.publishWithSeq(topic, hint, &roomSeq,
                                                         [&](uint64_t seq) {
                                                             built = std::make_shared<const std::string>(
                                                                 build(seq));
                                                             return built;
                                                         });
        if (!ok)
        {
            // ⚠️ 本地入队被拒（背压）时【不能】再广播集群、也不能落库。
            //
            // 原实现三条路都走：本地丢弃 + 集群广播 + Kafka 落库。于是
            // 发送者收到 503「消息未投递」，而其他实例的订阅者实际收到了这条
            // 消息、历史里也落了盘 —— 跨实例语义自相矛盾，客户端按 503 重发
            // 还会在别的实例上产生重复。要么整条消息都不发，要么就不该回 503。
            // 这里选择前者：以「本地是否接受」作为整条消息的统一裁决点。
            //
            // 序号也一并作废：被拒的消息不占号（RoomRegistry 的提交点在
            // 所有 return false 之后），所以 roomSeq 保持 0，接收端不会误报缺口。
            return false;
        }
        if (broadcastToCluster && built)
        {
            // 广播出去的是【带序号的那一份】—— 别的实例的订阅者也要靠它做游标，
            // 拿未带序号的原文会导致跨实例的客户端游标永远停在 0。
            publishToCluster(core, topic, *built, /*toUser=*/{}, roomSeq);
        }
        // ⚠️ Kafka 落库【不在这里】做：落库信封需要「这条消息是谁发的、是聊天
        //    还是入群/退群公告」这类上下文，而 fanOutRoom 只拿到已序列化好的
        //    客户端 VO，无法反推。把落库交给调用方（persistRoomMessage），
        //    它手里有全部上下文，并且能用房间名做分区键。
        if (outRoomSeq != nullptr)
        {
            *outRoomSeq = roomSeq;
        }
        return true;
    }

    static std::string buildNoticeJson(int code, const char* name, const char* message)
    {
        chatMessageVo vo{};
        vo.code = code;
        vo.name = name;
        vo.message = message;
        // 本函数构造的全部是「系统提示」：503 未投递 / 404 不在线 / -1 协议错误 /
        // 过载通知。客户端据此直接走系统提示样式，不必再猜文案。
        vo.type = "notice";
        std::string json{};
        (void)glz::write_json(vo, json);
        return json;
    }

    // 过载时明确告知客户端消息未投递，避免「静默丢消息」让上层误以为已送达。
    //
    // ⚠️ 调用方必须先过 Subscriber::shouldSendOverloadNotice() 限流。
    // 本函数本身不做限流，逐条调用会把丢弃路径变得和投递一样贵
    // （实测 14.36M 次丢弃 → 14.36M 次额外 send）。
    static void sendOverloadNotice(const WebSocketConnectionPtr& conn)
    {
        if (conn && conn->connected())
        {
            conn->send(buildNoticeJson(503, "系统通知", "服务器繁忙，消息未投递，请稍后重试"));
        }
    }
};
