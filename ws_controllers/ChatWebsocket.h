#pragma once
#include <drogon/WebSocketController.h>
#include "kafka/KafkaManager.h"
#include <glaze/glaze.hpp>
#include <drogon/HttpAppFramework.h>
#include "utils/retry_utils.h"
#include "utils/RateLimitedLog.h"
#include "parallel_hashmap/phmap.h"
#include "RoomRegistry.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <format>
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

        // 注册定时任务：空闲超时连接驱逐 + 房间指标采集 + 开关热更新。
        //
        // 回调按值捕获 core_（shared_ptr），而不是捕获 this：
        // 回调持有的引用会让 Core 活到本次回调结束，因此即使控制器已析构、
        // 或静态析构顺序与预期不同，也不会出现 use-after-free。
        // （原先捕获 this 且丢弃了 TimerId，退出阶段存在悬空访问窗口。）
        HttpAppFramework::instance().getLoop()->runEvery(5.0, [core = core_] {
            checkAndEvictIdleConnections(*core);
            publishRoomMetrics(*core);
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
        }

        // 房间注册表：按订阅者分片 + 多核并行扇出，严格保序。
        // 取代了原先的 drogon::PubSubService（它单线程扇出，房间越大越慢）
        // 与 RoomSerialDispatcher（保序职责已内聚到 RoomRegistry 的房间级发布锁）。
        RoomRegistry roomRegistry;

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
    static void publishToCluster(const Core& core, const std::string& topic,
                                 const std::string& json, const std::string& toUser = {});

    // 把一条已序列化的私聊消息投递给【本实例上】该用户名的所有在线会话。
    // 返回投递到的会话数（0 = 本实例上没有该用户名的在线会话）。
    //
    // sender 用来判断「发送者本人是否也在目标会话列表里」（同昵称多端）：
    // 在，说明他已经收到自己那一份，调用方不必再补一帧回显。
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
                                       bool* echoedToSender = nullptr);
    // 形参用 string_view：开关关闭时（压测/开发常态）调用方不必先构造 std::string，
    // 避免「已经拷贝完了才在函数里 short-circuit」这种白付的分配。
    // 需要所有权的地方（TBB worker）在 lambda 捕获里再落成 std::string。
    static void produceKafkaAsync(std::string_view topicName, std::string_view payload);

    // Kafka 落库开关的缓存值（缺省 false = fail-safe，见 .cc 里的说明）。
    //
    // 用原子量而不是 `static const`：后者只在首次调用时求值一次，
    // 于是「改 config.json 不生效、必须重启」—— 压测时切换落库开关很烦。
    static std::atomic<bool>& kafkaPersistenceEnabled() noexcept;

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
    };

    // 扇出参数：默认值适配本机，可用环境变量覆盖（便于压测 A/B 调参，不依赖配置加载时序）
    static RoomRegistry::Options makeFanoutOptions()
    {
        RoomRegistry::Options opt;
        // 默认不启用扇出线程池（opt.fanoutThreads 保持 0 = 单分片内联扇出）。
        // 多分片并行扇出是实验特性：保序正确性已由单测覆盖，但回声饱和压测下
        // 吞吐不稳定且低于内联路径，需要时用 LOONG_WS_FANOUT_THREADS 显式打开。
        auto readEnv = [](const char* key, size_t& out) {
            if (const char* v = std::getenv(key); v != nullptr && *v != '\0')
            {
                try
                {
                    out = static_cast<size_t>(std::stoull(v));
                }
                catch (...)
                {
                }
            }
        };
        readEnv("LOONG_WS_MAX_SHARDS", opt.maxShards);
        readEnv("LOONG_WS_SUBS_PER_SHARD", opt.subsPerShard);
        readEnv("LOONG_WS_FANOUT_THREADS", opt.fanoutThreads);
        readEnv("LOONG_WS_INLINE_MAX_SUBS", opt.inlineMaxSubs);
        readEnv("LOONG_WS_BACKLOG_PER_SHARD", opt.backlogPerShard);
        readEnv("LOONG_WS_DRAIN_BATCH", opt.drainBatch);
        readEnv("LOONG_WS_WORKER_SPIN_ROUNDS", opt.workerSpinRounds);
        return opt;
    }

    // 下面两个都由 5 秒定时任务驱动，因此做成静态函数 + 显式传入 Core：
    // 定时器回调捕获的是 core_（shared_ptr），不依赖控制器的生命周期。
    static void checkAndEvictIdleConnections(Core& core);

    // 把 RoomRegistry 的房间侧快照与扇出分组计数推送到 Prometheus registry。
    // 由 5 秒定时任务驱动：/metrics 抓取时就不必再去加房间表的锁，
    // 代价是最多 5 秒的滞后（gauge 类指标可以接受；counter 看增量也不受影响）。
    static void publishRoomMetrics(const Core& core);

    // 全局单调消息序号（进程内唯一）。
    // 原先用 subscriber.id_（RoomRegistry 的房间内自增订阅号）当消息 id ——
    // 同一发送者的所有消息 id 相同、跨房间重复，客户端若拿它做去重/排序会错乱。
    static uint64_t nextMessageId() noexcept
    {
        static std::atomic<uint64_t> seq{0};
        return seq.fetch_add(1, std::memory_order_relaxed) + 1;
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
    bool fanOutRoom(Core& core, const std::string& topic, const std::string& json,
                    bool broadcastToCluster = true,
                    const RoomRegistry::RoomHandle& hint = {})
    {
        const bool ok = core.roomRegistry.publish(topic, json, hint);
        if (broadcastToCluster)
        {
            publishToCluster(core, topic, json);
        }
        // Kafka 持久化：由 custom_config.enable_kafka_persistence 控制，
        // 关闭时 produceKafkaAsync 首行即短路返回（零开销）。
        // 压测/开发环境务必置 false —— 只生产不消费会把磁盘写满。
        produceKafkaAsync("chat_messages_topic", json);
        return ok;
    }

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
    };

    struct chatMessageVo
    {
        int code = -1;
        uint64_t id = 0;
        std::string name;
        std::string message;
    };

    static std::string buildNoticeJson(int code, const char* name, const char* message)
    {
        chatMessageVo vo{};
        vo.code = code;
        vo.name = name;
        vo.message = message;
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
