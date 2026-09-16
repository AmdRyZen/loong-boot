#include "ChatWebsocket.h"
#include "utils/redisUtils.h"
#include "utils/ConfigPath.h"
#include "coroutinePool/TbbCoroutinePool.h"
#include "utils/PrometheusMetrics.h"
//#include "user.pb.h"
#include <glaze/glaze.hpp>
#include <drogon/HttpAppFramework.h>
#include "utils/retry_utils.h"
#include <atomic>
#include <fstream>
#include <json/json.h>
#include <vector>
#include <drogon/nosql/RedisSubscriber.h>

// 注：Subscriber / Session / Core 的定义都在 ChatWebsocket.h 里。
// Subscriber 原先定义在本文件，为了让定时器与启动回调能够按值捕获 Core
//（而不是捕获 this）而整体上移到头文件。

std::atomic<bool>& ChatWebsocket::kafkaPersistenceEnabled() noexcept
{
    // 缺省 false（fail-safe）：配置里漏写 enable_kafka_persistence 时【不】往 Kafka 写。
    // 两个方向的代价不对称 —— 默认 true 会把「配置里忘了写」变成「只产不消把磁盘写满」，
    // 而默认 false 的代价只是「以为在落库其实没落」，且一查 /metrics 就看得见。
    static std::atomic<bool> enabled{false};
    return enabled;
}

std::atomic<bool>& ChatWebsocket::messageDedupEnabled() noexcept
{
    // 缺省 false。理由见头文件 Core::dedupSeen：去重的前提是「客户端为每条消息
    // 生成唯一 key」，而本工程既有的 chat.html 把 key 填成了房间名 ——
    // 默认打开等于把「客户端还没升级」变成「同一房间内该用户的消息被静默吞掉」。
    // 这个方向的错误不可接受，所以默认必须是关，由部署方确认客户端已升级后再打开。
    static std::atomic<bool> enabled{false};
    return enabled;
}

void ChatWebsocket::reloadSwitches()
{
    // 由构造函数与 5 秒定时任务调用。做成可重读是为了避免「改配置必须重启」——
    // 压测时切换落库开关不该需要重新起进程。
    //
    // ⚠️ 必须重读【磁盘上的配置文件】，不能读 drogon::app().getCustomConfig()：
    // 后者是启动时解析出来的内存副本，进程跑起来之后永远不变 ——
    // 拿它做热更新等于什么都没做（实测：改完文件后观察用的 gauge 一直不动）。
    //
    // 注：enable_cluster_bus 刻意【不】走这条路 —— 它在构造期决定要不要注册
    // Redis 订阅，注册之后无法中途注销，热更新只会造成「开关说 false 但总线还活着」
    // 这种自相矛盾的状态。详见 clusterBusEnabled()。
    bool parsed = false;
    bool kafka = false;
    bool dedup = false;
    try
    {
        Json::Value root;
        Json::CharReaderBuilder builder;
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        std::string errs;
        std::ifstream ifs(Config::filePath(), std::ios::binary);
        parsed = ifs && Json::parseFromStream(builder, ifs, &root, &errs);
        if (parsed)
        {
            const auto& custom = root["custom_config"];
            if (custom.isMember("enable_kafka_persistence"))
            {
                kafka = custom["enable_kafka_persistence"].asBool();
            }
            if (custom.isMember("enable_message_dedup"))
            {
                dedup = custom["enable_message_dedup"].asBool();
            }
        }
        else
        {
            // 读不到就【保持当前值】并只告警一次。
            // 不能每 5 秒刷一行 —— 那正好是本项目反复踩过的「热路径日志放大」。
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true, std::memory_order_relaxed))
            {
                LOG_WARN << "Cannot re-read config file '" << Config::filePath()
                         << "' for hot switch reload: " << (errs.empty() ? "open failed" : errs)
                         << " — switches keep their current value";
            }
        }
    }
    catch (...)
    {
        parsed = false;
    }

    if (!parsed)
    {
        // ⚠️ 关键：解析失败必须【直接返回】，绝不能继续走到下面的 exchange()。
        // 原实现让 kafka 保持初值 false 并照样 exchange，于是
        // 「配置文件被编辑器/部署工具短暂替换」的 5 秒窗口会把已经开启的
        // 落库开关静默改回 false —— 与上面那句「switches keep their current
        // value」的日志完全相反，排障时极具误导性。
        // gauge 仍按当前生效值刷新一次，保证 /metrics 不撒谎。
        Metrics::PrometheusRegistry::instance().setSwitchStates(
            kafkaPersistenceEnabled().load(std::memory_order_relaxed),
            messageDedupEnabled().load(std::memory_order_relaxed));
        return;
    }

    // exchange 而不是 store：只有真的发生跳变才留痕。
    // 热更新必须可审计 —— 否则「谁在什么时候把落库打开了」无从查起。
    // 首次调用若配置就是 true，也会打印一行（false -> true），正好说明生效值。
    const bool prev = kafkaPersistenceEnabled().exchange(kafka, std::memory_order_relaxed);
    if (prev != kafka)
    {
        // LOG_WARN 宏没有级别判断，永远输出 —— 审计信息不能被 log_level 过滤掉。
        LOG_WARN << "Kafka persistence switch changed: " << (prev ? "true" : "false") << " -> "
                 << (kafka ? "true" : "false") << " (custom_config.enable_kafka_persistence)";
    }

    // 去重开关同样留痕。这条尤其重要：打开它会让「客户端 key 不唯一」的客户端
    // 开始丢消息，事后必须能查到是什么时候被打开的。
    const bool prevDedup = messageDedupEnabled().exchange(dedup, std::memory_order_relaxed);
    if (prevDedup != dedup)
    {
        LOG_WARN << "Message dedup switch changed: " << (prevDedup ? "true" : "false") << " -> "
                 << (dedup ? "true" : "false") << " (custom_config.enable_message_dedup)";
    }

    Metrics::PrometheusRegistry::instance().setSwitchStates(kafka, dedup);
}

void ChatWebsocket::produceKafkaAsync(std::string_view topicName, std::string_view payload,
                                      std::string_view key)
{
    // 开关检查必须在【任何字符串构造之前】。
    //
    // 形参原先按值传 std::string：即便 Kafka 关闭、函数在这里立刻 return，
    // 实参的堆拷贝也已经发生完了 —— 热路径上每条消息白付一次分配。
    // 改成 string_view 后，关闭态下这次调用只是两次指针/长度赋值。
    //
    // ⚠️ 但这只挡住了【本函数自己】那一层。调用方（persistRoomMessage /
    // persistDirectMessage）还要先填 chatPersistVo 并 buildPersistJson ——
    // 那份开销更大（实测 222.8 ns/条），所以那两个函数各自在首行也做了同样的检查。
    // 两处都要保留：这里是独立入口的兜底，那里才是省掉大头的地方。
    //
    // 开关本身从「static const 一次性求值」改成原子量：原实现改配置永不生效，
    // 只能重启进程（压测时切换落库开关很烦）。值由 reloadSwitches() 维护。
    if (!kafkaPersistenceEnabled().load(std::memory_order_relaxed))
    {
        return; // 开发或压测模式下跳过 Kafka 写入，保持极致 CPU 吞吐
    }

    // 只有在真正要投递时才把视图落成所有权字符串（TBB worker 会活过调用方栈帧）。
    // 原实现另有一个 `const std::string topicForLog = topicName;` 的无条件拷贝，
    // 仅仅为了失败日志 —— 现在日志直接打视图，拷贝彻底消失。
    //
    // 背压：TBB 池积压超过上限时 submit 会返回 false。
    // 原实现忽略返回值 → 消息被静默丢弃，无日志无指标，上层误以为已落库。
    const bool accepted = TbbCoroutinePool::instance().submit(
        [topic = std::string(topicName), payload = std::string(payload),
         partitionKey = std::string(key)] {
            // ⚠️ TbbCoroutinePool 内部对任务包了 catch(...)，lambda 里抛出的任何异常
            // 都会被【静默吞掉】（无日志无指标）。KafkaManager::getTopic 在未初始化 /
            // topic 句柄创建失败时抛 runtime_error —— 不在这里自己接住的话，
            // 消息就是无声消失。所以本 lambda 内的一切工作都必须自己兜异常。
            // 终态失败日志的限流器（按秒，行内带上被压掉的条数）。
            //
            // 为什么必须限流：本 lambda 的两条失败路径（getTopic 抛异常 / produce 重试后仍未成功）
            // 触发频率都与【消息流量】成正比 —— KafkaManager 或 broker 不可用时每条消息都走。
            // 与 overloadLogLimiter_ / jsonParseErrorLogLimiter_ 同一把尺子。
            //
            // 刻意用函数内静态量而不是实例成员：produceKafkaAsync 是 static 成员函数拿不到实例；
            // 且「落库失败」是全局事件，跨 topic 共享一个窗口正是想要的语义。
            static loong::log::RateLimiter failLogLimiter{1000};

            // 两条失败路径共用同一个限流器 —— 否则它们各自限流、合起来仍可能被冲垮。
            // 必须在 allow() 返回 true 之后才调 takeSuppressed()（见 RateLimitedLog.h）。
            // 注意：这里才构造日志字符串，成功路径一行都不打、也不分配。
            const auto logTerminalFailure = [&](const char* reason, bool severe) {
                if (!failLogLimiter.allow())
                {
                    return;
                }
                const std::string line =
                    std::string("Kafka persist failed for topic '") + topic + "': " + reason + ", " +
                    std::to_string(1 + failLogLimiter.takeSuppressed()) +
                    " occurrence(s) since last log (total: ws_kafka_produce_failed_total)";
                if (severe)
                {
                    LOG_ERROR << line;
                }
                else
                {
                    LOG_WARN << line;
                }
            };

            rd_kafka_topic_t* topicPtr = nullptr;
            try
            {
                topicPtr = kafka::KafkaManager::instance().getTopic(topic);
            }
            catch (const std::exception& e)
            {
                Metrics::PrometheusRegistry::instance().recordKafkaProduceFailed();
                logTerminalFailure(e.what(), /*severe=*/true);
                return;
            }

            // ── 单次投递，不做 sleep 重试 ───────────────────────────────────────
            //
            // 原实现用 retryWithSleep（默认 3 次 × 100ms）在【TBB worker 线程里】
            // std::this_thread::sleep_for。两个问题：
            //
            // ① 它真的会阻塞池。aop/Application.h 用
            //    tbb::global_control(max_allowed_parallelism, hardware_concurrency())
            //    把 TBB 并行度锁死在核数上，所以「几个 worker 在 sleep」等于
            //    「池少几个执行槽」。broker 抖动时所有 worker 一起进重试 ⇒
            //    池停止抽干 ⇒ activeTasks_ 涨过 32768 ⇒ submit 返回 false ⇒
            //    ws_kafka_persist_dropped_total 飙升（丢的是落库，消息本身已投递）。
            //    TBB worker 应当非阻塞。
            //
            // ② 重试本身几乎没有价值。safeProduce 返回 false 只有三种成因：
            //    · MSG_SIZE_TOO_LARGE —— 永久失败，重试无意义（WS 上限 128KB，远小于 1MB）；
            //    · UNKNOWN_TOPIC —— 只有 allow.auto.create.topics=false 才可能，本工程未设；
            //    · QUEUE_FULL —— 需要本地队列积满 100 万条。按 queue.buffering.max.messages
            //      = 1000000 与 delivery.timeout.ms = 30000 推算，稳态积压 ≈ 速率 × 30s，
            //      要积满得速率 ≥ ~33,333 条/秒 持续 30 秒以上；真到那一步，
            //      睡 100ms 也排不空队列（排空本身是 30 秒量级）。
            //
            // 所以不重试，失败即记终态（限流日志 + 计数）。这既消除了池阻塞，
            // 也不损失可观测性 —— 原先「重试耗尽」与「永久性错误放弃」本来就
            // 合并成同一条终态日志输出。
            if (kafka::KafkaManager::safeProduce(topicPtr, payload, partitionKey))
            {
                return;
            }

            const rd_kafka_resp_err_t lastErr = rd_kafka_last_error();
            Metrics::PrometheusRegistry::instance().recordKafkaProduceFailed();
            logTerminalFailure(rd_kafka_err2str(lastErr), /*severe=*/false);
        });

    if (!accepted)
    {
        // 注意用 recordKafkaPersistDropped 而不是 recordWsMessageDropped：
        // 这条消息【已经投递给订阅者了】，丢的只是异步落库。
        // 混进 ws_messages_dropped_total 会让「实时消息没投出去」和
        // 「投出去了但没落库」这两类完全不同的事故变成一个数。
        Metrics::PrometheusRegistry::instance().recordKafkaPersistDropped();
        LOG_WARN << "TBB pool saturated, dropped Kafka persistence for topic: " << topicName;
    }
}

void ChatWebsocket::handleNewMessage(const WebSocketConnectionPtr& wsConn, std::string&& msg, const WebSocketMessageType& type)
{
    try
    {
        if (type == WebSocketMessageType::Ping)
        {
            if (wsConn->hasContext())
            {
                auto& subscriber = wsConn->getContextRef<Subscriber>();
                subscriber.touch();
            }
            // 不再手动回 Pong：drogon 的 WebSocketConnectionImpl 收到对端的 Ping 帧时
            // 已经自动回过一帧（见 lib/src/WebSocketConnectionImpl.cc 的 Ping 分支），
            // 这里再回一次会让客户端收到两个 Pong，影响它按 Pong 计数做的 RTT/存活判断。
            // 本分支只负责续期。
            LOG_DEBUG << "Received a ping";
            return;
        }

        if (type == WebSocketMessageType::Pong)
        {
            // 客户端回的 Pong 会走到这里（drogon 只在收到【对端】帧时才回调），
            // 据此续期 —— 所以「只收不发」的标准客户端不会被 60s 空闲驱逐误杀。
            if (wsConn->hasContext())
            {
                auto& subscriber = wsConn->getContextRef<Subscriber>();
                subscriber.touch();
            }
            return;
        }

        if (type == WebSocketMessageType::Close)
        {
            // 同样是「每连接一条」的诊断噪声，不该占 INFO
            LOG_DEBUG << "Received a Close";
            return;
        }

        if (!msg.empty())
        {
            chatMessageDto msg_dto{};
            if (glz::read_json(msg_dto, msg))
            {
                Metrics::PrometheusRegistry::instance().recordWsJsonParseError();

                // 回一个可读的错误。原先回的是默认构造的空 VO，客户端只能收到
                // {"code":-1,"id":0,"name":"","message":""} —— 既不知道原因，
                // 也没法对应到具体请求（与 503/404 的 code 体系也不一致）。
                wsConn->send(buildNoticeJson(-1, "系统通知", "消息格式错误：无法解析 JSON"),
                             WebSocketMessageType::Text);

                // 触发频率完全由客户端决定（狂发非法负载即可无限刷），必须限流。
                // 是真错误所以不静默。行内给出「本次覆盖多少条」；
                // 累计总量看 ws_json_parse_errors_total（限流会让日志少报，
                // 但 counter 不会）。
                if (jsonParseErrorLogLimiter_.allow())
                {
                    LOG_ERROR << "Failed to parse JSON message: "
                              << (1 + jsonParseErrorLogLimiter_.takeSuppressed())
                              << " occurrence(s) since last log "
                              << "(total: ws_json_parse_errors_total)";
                }
                return;
            }

            // hasContext() 必须判：连接在 handleNewConnection 中途失败时上下文可能未落，
            // getContextRef 会直接解引用空指针（UB），而不是抛异常。
            //
            // 用 connected() 而不是 !disconnected()：drogon 在 Connecting/Disconnecting
            // 中间态下 send 会被 sendInLoop 静默丢弃，「尚未彻底关闭」并不等于「可投递」。
            if (wsConn->connected() && wsConn->hasContext())
            {
                auto& subscriber = wsConn->getContextRef<Subscriber>();
                subscriber.touch();
                const std::string& topic = subscriber.topic_;
                const std::string& senderName = subscriber.userName_;

                const std::string_view action{msg_dto.action};

                // 应用层心跳：touch() 已在上面做过，这里既不广播也不回包。
                // （chat.html 每 25 秒发一次，用于探测「协议栈还活着但前端 JS 卡死」，
                //  属纵深防御；正常保活靠 drogon 的协议层 Ping/Pong。）
                if (action == "ping")
                {
                    return;
                }

                // 未知 action 原先被静默丢弃：客户端拼错字段名/拼错值时，
                // 服务端既不回包也不记日志，现象是「消息发出去没有任何反应」，
                // 排障只能靠猜。这里明确回一个可读错误。
                if (action != "message")
                {
                    wsConn->send(buildNoticeJson(-1, "系统通知", "未知的 action，仅支持 message / ping"),
                                 WebSocketMessageType::Text);
                    return;
                }

                // 空内容不广播：原先空 msgContent 也会被当成一条正常消息
                // 广播给全房间（还占用一个消息 id），属于无效流量放大。
                if (msg_dto.msgContent.empty())
                {
                    wsConn->send(buildNoticeJson(-1, "系统通知", "消息内容不能为空"),
                                 WebSocketMessageType::Text);
                    return;
                }

                {
                    Metrics::PrometheusRegistry::instance().recordWsMessage();

                    // 全局唯一消息 id 在这里分配一次（原先在两个分支里各调一次）。
                    // 放在判重【之前】：id 只是 snowflake 序号，中间浪费一个号没有
                    // 任何语义影响（客户端不会拿它做连续性断言），但省掉了
                    // 「在去重表锁里分配 id」这件事。
                    const uint64_t msgId = nextMessageId();

                    // ── 消息去重（客户端重发幂等，B2）────────────────────────────
                    //
                    // 位置：在所有格式校验之后、任何投递/落库之前。
                    //   太早 → 格式错误的重发也会被当成重复，客户端就永远收不到那条错误提示；
                    //   太晚 → 重复消息已经广播出去了，去重就没有意义。
                    if (messageDedupEnabled().load(std::memory_order_relaxed) && !msg_dto.key.empty())
                    {
                        // 键 = (发送者昵称, 客户端 dto.key)。必须带上发送者：
                        // key 由客户端提供，两个客户端撞上同一个值是完全可能的。
                        // 分隔符用 \x1f（US，单元分隔符）—— 昵称里出现它的概率远低于
                        // '|' 之类可打印字符，且无需转义。
                        std::string dedupKey;
                        dedupKey.reserve(senderName.size() + msg_dto.key.size() + 1);
                        dedupKey.append(senderName).push_back('\x1f');
                        dedupKey.append(msg_dto.key);

                        const int64_t nowMsVal = loong::chat::nowMs();
                        uint64_t dupOfId = 0;
                        {
                            std::lock_guard lock(core_->dedupMtx);
                            const auto it = core_->dedupSeen.find(dedupKey);
                            if (it != core_->dedupSeen.end())
                            {
                                dupOfId = it->second.msgId;
                                // 刷新时间戳：客户端在重试退避里连发多次时，
                                // 不应该因为「首次处理已过去 120 秒」而在重试中途被放行。
                                it->second.atMs = nowMsVal;
                            }
                            else
                            {
                                if (core_->dedupSeen.size() >= Core::kMaxDedupEntries)
                                {
                                    // 到上限整体清空（与 roomSeqWatermark_ 同一手法）。
                                    // 宁可让一小段窗口里的重发不再被识别，也不要让
                                    // 一个优化设施变成内存泄漏。
                                    core_->dedupSeen.clear();
                                }
                                core_->dedupSeen.emplace(std::move(dedupKey),
                                                         Core::DedupEntry{msgId, nowMsVal});
                            }
                        }

                        if (dupOfId != 0)
                        {
                            // 重发：不再广播、不再落库，但【仍然回显】。
                            //
                            // 为什么必须回显：客户端重发通常正是因为它没收到上一次的回显
                            //（闪断重连 / 超时）。若这里静默丢弃，客户端的重试循环永远
                            // 等不到应答，会一直重发下去。
                            //
                            // 回显里带的是【首次处理时分配的 id】而不是本次的新号 ——
                            // 客户端按 id 去重时能把两份回显认成同一条，
                            // 而房间里的其他人始终只看到一条。
                            chatMessageVo echo{};
                            echo.code = 200;
                            echo.id = dupOfId;
                            echo.name = std::string_view(senderName);
                            // 私聊的显示文本带 "[私聊] " 前缀，必须与首次处理保持一致，
                            // 否则客户端看到的重发回显与首次回显文案不同。
                            echo.message =
                                msg_dto.toUser.empty()
                                    ? msg_dto.msgContent
                                    : std::format("[私聊] {}", std::string_view(msg_dto.msgContent));
                            echo.type = "message";

                            std::string echoJson{};
                            (void)glz::write_json(echo, echoJson);
                            wsConn->send(echoJson, WebSocketMessageType::Text);
                            Metrics::PrometheusRegistry::instance().recordWsMessageDeduped();
                            return;
                        }
                    }

                    // 判断是否为点对点私聊 (toUser 非空)
                    if (!msg_dto.toUser.empty())
                    {
                        const std::string targetUser(msg_dto.toUser);

                        chatMessageVo msg_vo{};
                        msg_vo.code = 200;
                        msg_vo.id = msgId; // 与去重表里登记的是同一个号（见上）
                        msg_vo.name = std::string_view(senderName);
                        msg_vo.message = std::format("[私聊] {}", std::string_view(msg_dto.msgContent));
                        msg_vo.type = "message";

                        std::string json{};
                        (void)glz::write_json(msg_vo, json);

                        // ① 本实例本地投递（多端登录：该昵称的所有在线会话都收到）
                        bool echoedToSender = false;
                        size_t backpressured = 0;
                        const size_t localDelivered =
                            deliverDirectLocally(*core_, targetUser, json, wsConn, &echoedToSender,
                                                 &backpressured);

                        // ①' 背压：本实例的在途投递额度已满 ⇒ 整条消息【不投、不广播、
                        //     不落库】，明确回 503。
                        //
                        // 与房间扇出同一个裁决点原则（见 fanOutRoom 里那段注释）：
                        // 若这里仍继续广播集群 + 落库，发送者收到 503 而别的实例
                        // 已经投出去了 —— 客户端按 503 重发就会在那边产生重复。
                        //
                        // ⚠️ 必须先于 404 判定：两者都是 localDelivered == 0，
                        //    但「本实例有这个人、只是额度满了」和「本实例没这个人」
                        //    是两件完全不同的事。报 404 会让客户端以为对方不在线。
                        if (backpressured > 0)
                        {
                            auto& metrics = Metrics::PrometheusRegistry::instance();
                            // 消息级计数进 ws_messages_dropped_total（与其他丢弃同口径），
                            // 会话级计数进 ws_direct_backpressured_total（原因细分）。
                            metrics.recordWsMessageDropped();
                            if (subscriber.shouldSendOverloadNotice())
                            {
                                sendOverloadNotice(wsConn);
                            }
                            else
                            {
                                metrics.recordWsOverloadNoticeSuppressed();
                            }
                            // 与房间丢弃共用限流器：同属「过载」这一种现象。
                            if (overloadLogLimiter_.allow())
                            {
                                LOG_WARN << "Private delivery backpressured, message dropped for user "
                                         << targetUser << ": "
                                         << (1 + overloadLogLimiter_.takeSuppressed())
                                         << " occurrence(s) since last log "
                                         << "(total: ws_direct_backpressured_total)";
                            }
                            return;
                        }

                        // 本实例查无此人，且【集群总线实际不可用】→ 可以确定不在线，明确告知。
                        //
                        // 判定用运行时标志 clusterBusReady 而不是配置开关 clusterBusEnabled()：
                        // 配置写 true 但 Redis 没起来/订阅失败时，总线其实是死的。
                        // 那种情况下若仍按「在线状态不可知」跳过 404，就会回一个
                        // 200 假 ACK —— 发送者以为投递成功，消息却哪儿都没去。
                        // 宁可诚实回 404，也不给假成功。
                        if (localDelivered == 0 &&
                            !core_->clusterBusReady.load(std::memory_order_acquire))
                        {
                            wsConn->send(buildNoticeJson(
                                404, "系统通知", std::format("用户 {} 当前不在线", targetUser).c_str()));
                            return;
                        }

                        // ② 回显给发送者：他本人不在目标会话列表里（没收到自己那一份）才补。
                        //    注意条件是「没回显过」而不是「本地投递成功过」——
                        //    跨实例投递的结果同步不可知，但发送方自己的聊天窗口
                        //    无论如何都该出现这条消息。
                        if (!echoedToSender)
                        {
                            wsConn->send(json);
                        }

                        // ③ 跨实例投递。
                        //
                        // 昵称即身份：与本工程「同一昵称多端并存」的既有约定一致，
                        // 因此【总是】广播给其他实例，让它们各自投给本地的同名会话。
                        // 若改成「本地命中就不广播」，则同一昵称在别的实例上的会话
                        // 会漏收 —— 与房间广播的语义（全实例可见）不一致。
                        //
                        // 代价：同实例内的 1:1 私聊也要过一次 Redis。若私聊量远超房间消息，
                        // 更省的做法是维护 userName→实例 的路由表（需心跳与失效处理），
                        // 但那是另一套复杂度，当前规模下不值得。
                        publishToCluster(*core_, /*topic=*/{}, json, targetUser);

                        // 生产环境持久化：私聊消息异步推至 Kafka 私聊主题。
                        // 只在发送方实例落库（接收方实例投递时不落），避免跨实例重复。
                        // 受 custom_config.enable_kafka_persistence 控制，关闭时零开销。
                        //
                        // 落的是【自描述信封】而不是客户端 VO：带 toUser/type/sender/
                        // originInstance/ts，回放端才能还原「谁发给谁」。分区键取
                        // 「排序后的双方昵称」，保证同一会话两个方向落同一分区。
                        persistDirectMessage(*core_, senderName, targetUser, msg_vo, msg_dto.key);
                        return;
                    }

                    // 默认房间广播模式：直接把已序列化的 payload 交给 RoomRegistry。
                    // 保序由 RoomRegistry 内部的「房间级发布锁 + 分片 FIFO 队列」保证，
                    // 扇出本身在多核上并行，所以这里不再需要额外的串行派发器中转，
                    // 也不再经 TBB 池（多线程池会打乱入队顺序，下游再怎么串行都救不回来）。
                    chatMessageVo msg_vo{};
                    msg_vo.code = 200;
                    msg_vo.id = msgId; // 与去重表里登记的是同一个号（见上）
                    msg_vo.name = std::string_view(senderName);
                    msg_vo.message = std::move(msg_dto.msgContent);
                    msg_vo.type = "message";

                    std::string json{};
                    (void)glz::write_json(msg_vo, json);

                    // 1. 本实例本地房间广播（分片并行扇出）
                    // 2. 分布式总线：同步广播给集群其他实例（带上房间级序号）
                    // 3. 生产环境持久化：异步投递到 Kafka 历史消息流（分区键 = 房间名）
                    // 带上本连接缓存的房间句柄：hint 与 topic 同源（都是本订阅者的字段），
                    // 因此 publish 可以安全地跳过全局房间表锁。
                    uint64_t roomSeq = 0;
                    if (!fanOutRoom(*core_, topic, json, /*broadcastToCluster=*/true,
                                    subscriber.room_, &roomSeq))
                    {
                        auto& metrics = Metrics::PrometheusRegistry::instance();
                        metrics.recordWsMessageDropped();

                        // 告知客户端「未投递」是设计约定（背压不得静默），但必须限流：
                        // 逐条回通知会把丢弃路径变得和投递一样贵。
                        if (subscriber.shouldSendOverloadNotice())
                        {
                            sendOverloadNotice(wsConn);
                        }
                        else
                        {
                            metrics.recordWsOverloadNoticeSuppressed();
                        }

                        // 逐条 WARN 在饱和时会写出 GB 级日志（实测 30s → 1.6GB /
                        // 13.8M 行），磁盘 I/O 反过来拖垮扇出。这里按秒限流，
                        // 行内给出「本次覆盖多少条」；累计总量看
                        // ws_messages_dropped_total（counter 不受限流影响）。
                        if (overloadLogLimiter_.allow())
                        {
                            // 房间名与订阅者数是「慢消费者定位」的关键上下文：
                            // 原先这条日志既不说哪个房间、也不说那个房间多大，
                            // 看到刷屏只能靠 ws_room_max_backlog 猜。
                            // subscribersIn() 会加一次房间表共享锁 —— 这里每秒最多
                            // 一次（限流器已放行），可以接受；绝不能挪到限流之前。
                            LOG_WARN << "Room backlog full, message dropped: "
                                     << (1 + overloadLogLimiter_.takeSuppressed())
                                     << " occurrence(s) since last log in room '" << topic << "' ("
                                     << core_->roomRegistry.subscribersIn(topic)
                                     << " subscriber(s); total: ws_messages_dropped_total)";
                        }
                        // 注意：被拒时【不落库】（与 fanOutRoom 里集群广播的取舍一致）——
                        // 否则客户端按 503 重发会在历史里留下两条。
                        return;
                    }

                    // 落库放在投递成功之后：被背压拒绝的消息不落库（见上）。
                    persistRoomMessage(*core_, topic, senderName, msg_vo, roomSeq, "room",
                                       msg_dto.key);
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        // 只打日志的话，线上没有任何可累加的信号（日志可能被级别过滤或轮转掉）。
        Metrics::PrometheusRegistry::instance().recordWsHandlerException();
        LOG_ERROR << "Error in handleNewMessage: " << e.what();
    }
}

void ChatWebsocket::handleNewConnection(const HttpRequestPtr& req, const WebSocketConnectionPtr& wsConn)
{
    try
    {
        std::string topic = req->getHeader("room_name");
        if (topic.empty())
        {
            topic = req->getParameter("room_name");
            if (topic.empty())
            {
                topic = "default_room";
            }
        }
        std::string userName = req->getHeader("name");
        if (userName.empty())
        {
            userName = req->getParameter("name");
            if (userName.empty())
            {
                userName = "default_name";
            }
        }

        // 先落上下文：hasContext() 从此成为「连接已登记」的可靠标志。
        // 这样 connect/disconnect 指标必然成对，断开路径也能安全地判空后取值。
        auto subscriber = std::make_shared<Subscriber>();
        subscriber->topic_ = topic;
        subscriber->userName_ = userName;
        wsConn->setContext(subscriber);

        Metrics::PrometheusRegistry::instance().recordWsConnect();

        // 注册到房间注册表：分片直接持有连接指针，省掉 std::function 回调中转的开销。
        //
        // 同时登记「该连接所属的事件循环」：drogon 的 handleNewConnection 是在
        // 该连接的 IO 线程里同步调用的（HttpServer::websocketRequestHandling →
        // WebsocketControllerBinder::handleNewConnection），所以此刻取到的当前线程
        // loop 就是这条连接的归属 loop。扇出据此分组，每个目标 loop 每条消息只唤醒
        // 一次，投递在各自 loop 线程内完成 —— 唤醒次数由 O(订阅者数) 降为 O(loop 数)。
        //
        // 取不到 loop 时（返回 nullptr）句柄按「本线程」处理，即回退到逐份直接 send，
        // 语义与优化前一致，只是没有加速，不会出错。
        // ── 注册顺序：先登记「在线」，再订阅房间 ────────────────────────────
        //
        // 原实现相反（先 subscribe 再插 userNameToConn），于是存在一个窗口：
        // 新连接已经在房间里收消息了，但别人此刻给它发私聊会查不到条目 →
        // 回「用户不在线」。压测里快速反复建连可以把这个小窗口稳定放大。
        //
        // 反过来先登记则不会出现这种自相矛盾：条目一旦存在就说明该连接已建立，
        // 私聊投给它是安全的（send 到未完全就绪的连接由 drogon 静默丢弃，
        // 与 deliverDirectLocally 现有语义一致）。若后续 subscribe 抛异常，
        // catch 分支会 forceClose → handleConnectionClosed 按 conn 精确摘除，
        // 登记与清理仍然成对。
        {
            std::unique_lock lock(core_->connMutex);
            core_->userNameToConn[userName].push_back(Session{wsConn, subscriber});
            core_->connCount.fetch_add(1, std::memory_order_relaxed);
        }

        // 注册到房间注册表：分片直接持有连接指针，省掉 std::function 回调中转的开销。
        //
        // 同时登记「该连接所属的事件循环」：drogon 的 handleNewConnection 是在
        // 该连接的 IO 线程里同步调用的（HttpServer::websocketRequestHandling →
        // WebsocketControllerBinder::handleNewConnection），所以此刻取到的当前线程
        // loop 就是这条连接的归属 loop。扇出据此分组，每个目标 loop 每条消息只唤醒
        // 一次，投递在各自 loop 线程内完成 —— 唤醒次数由 O(订阅者数) 降为 O(loop 数)。
        //
        // 取不到 loop 时（返回 nullptr）句柄按「本线程」处理，即回退到逐份直接 send，
        // 语义与优化前一致，只是没有加速，不会出错。
        subscriber->id_ = core_->roomRegistry.subscribe(
            topic, wsConn, TrantorLoopHandle{trantor::EventLoop::getEventLoopOfCurrentThread()});

        // 同一个 loop 句柄再缓存到 Subscriber 上：房间扇出已经按它分组，
        // 私聊直投也要用（见 Subscriber::loop_ 与 deliverDirectLocally）。
        // 只取一次，终身复用 —— 连接所属 loop 在生命周期内不会改变。
        subscriber->loop_ = TrantorLoopHandle{trantor::EventLoop::getEventLoopOfCurrentThread()};

        // 缓存房间句柄：此后这条连接的每次发布都带上传回的句柄，publish 走快路径
        // 直接命中该房间，不再每条消息都去抢全局 roomsMtx_ 共享锁。
        // 房间被回收后句柄自动失效（retired），publish 会回退查表，无需在退订时清理。
        subscriber->room_ = core_->roomRegistry.acquireRoomHandle(topic);

        // 每连接一条的诊断信息，不是「有问题」的信号 → DEBUG
        LOG_DEBUG << "Added connection for user: " << userName << " Subscriber ID: " << subscriber->id_
                 << ", Topic: " << topic;

        chatMessageVo msg_vo;
        msg_vo.code = 200;
        msg_vo.id = nextMessageId();
        msg_vo.name = topic;
        msg_vo.message = std::format("欢迎 {} 加入我们 {}", userName, topic);
        msg_vo.type = "notice";

        // 使用普通string避免thread_local问题
        std::string json{};
        (void)glz::write_json(msg_vo, json);

        // 与聊天消息共用同一房间的发布锁，保证「入群公告」与其他消息的相对顺序稳定。
        //
        // broadcastToCluster = false：在线状态是本实例的本地事实。广播给其他实例后，
        // 那边的同名用户会收到与自己无关的「XX 已加入」；退群公告同理（见下）。
        uint64_t roomSeq = 0;
        if (!fanOutRoom(*core_, topic, json, /*broadcastToCluster=*/false, subscriber->room_,
                        &roomSeq))
        {
            Metrics::PrometheusRegistry::instance().recordWsMessageDropped();
            // 与消息丢弃共用同一个限流器：同属「房间积压」这一种过载现象。
            // 触发频率同样由客户端决定（反复快速建连/断连即可），不能逐条打。
            if (overloadLogLimiter_.allow())
            {
                LOG_WARN << "Room backlog full, dropped join notice for room " << topic << ": "
                         << (1 + overloadLogLimiter_.takeSuppressed())
                         << " occurrence(s) since last log (total: ws_messages_dropped_total)";
            }
        }
        else
        {
            // 公告也落库（type = "notice"），否则历史回放里会出现「有人说话但
            // 没人在房间里」的断裂。注意公告不跨实例广播，所以只有本实例写一条。
            persistRoomMessage(*core_, topic, userName, msg_vo, roomSeq, "notice");
        }
    }
    catch (const std::exception& e)
    {
        Metrics::PrometheusRegistry::instance().recordWsHandlerException();
        LOG_ERROR << "Error in handleNewConnection: " << e.what();

        // 半初始化状态不能留：连接若已建立却没登记完整，就主动关掉，让
        // handleConnectionClosed 走完整清理路径。
        //
        // 否则会出现「活着但不在 userNameToConn 里」的僵尸连接 ——
        // 空闲驱逐是按 userNameToConn 遍历的，看不到它；而它每帧都 touch()，
        // 于是永远不会被驱逐（原实现正是如此，会一直占着连接与内存）。
        if (wsConn)
        {
            wsConn->forceClose();
        }
    }
}

void ChatWebsocket::handleConnectionClosed(const WebSocketConnectionPtr& wsConn)
{
    try
    {
        // 未登记成功的连接直接放过，避免 getContextRef 解引用空上下文（UB）。
        // 这条路径与 handleNewConnection 的 setContext 失败路径对称：
        // 那时 recordWsConnect 也没记过，所以这里不记 disconnect 是正确的。
        if (!wsConn || !wsConn->hasContext())
        {
            LOG_WARN << "Closed a connection without subscriber context, skip cleanup";
            return;
        }

        const auto& subscriber = wsConn->getContextRef<Subscriber>();
        const std::string userName = subscriber.userName_;
        const std::string topic = subscriber.topic_;
        const auto id = subscriber.id_;

        // 只摘除「本连接自己」这一条记录：
        // 原实现按昵称直接 erase，同名的新连接会被旧连接的断开事件误删，
        // 导致新用户从此收不到私聊、且残留幽灵条目。
        bool removed = false;
        {
            std::unique_lock lock(core_->connMutex);
            if (const auto it = core_->userNameToConn.find(userName);
                it != core_->userNameToConn.end())
            {
                auto& sessions = it->second;
                const auto before = sessions.size();
                std::erase_if(sessions,
                              [&wsConn](const Session& s) { return s.conn == wsConn; });
                removed = sessions.size() != before;
                if (sessions.empty())
                {
                    core_->userNameToConn.erase(it);
                }
            }
        }
        if (removed)
        {
            core_->connCount.fetch_sub(1, std::memory_order_relaxed);
        }
        LOG_DEBUG << "Removed user: " << userName;

        // 先记 disconnect 再做 unsubscribe：unsubscribe 万一抛异常（例如分配失败），
        // 也不能让指标与本地登记失衡。recordWsConnect 已在建连路径记过，这里必须成对。
        Metrics::PrometheusRegistry::instance().recordWsDisconnect();

        try
        {
            core_->roomRegistry.unsubscribe(topic, id);
        }
        catch (const std::exception& e)
        {
            // RoomRegistry 内部有锁，抛异常只可能是分配失败；记录后继续走公告流程。
            // 残留条目由房间回收兜底，不会永久泄漏。
            Metrics::PrometheusRegistry::instance().recordWsHandlerException();
            LOG_ERROR << "unsubscribe failed for topic " << topic << ", ID: " << id << ": " << e.what();
        }
        LOG_DEBUG << "Unsubscribed from topic: " << topic << ", ID: " << id;

        chatMessageVo msg_vo;
        msg_vo.code = 200;
        msg_vo.id = nextMessageId();
        msg_vo.name = topic;
        msg_vo.message = std::format("{} 已离开 {}", userName, topic);
        msg_vo.type = "notice";

        // 使用普通string避免thread_local问题
        std::string json{};
        (void)glz::write_json(msg_vo, json);

        // 与聊天消息共用同一房间的发布锁，保证「离群公告」与其他消息的相对顺序稳定。
        // 同样不跨实例广播（理由见 handleNewConnection 的入群公告）。
        uint64_t roomSeq = 0;
        if (!fanOutRoom(*core_, topic, json, /*broadcastToCluster=*/false, subscriber.room_,
                        &roomSeq))
        {
            Metrics::PrometheusRegistry::instance().recordWsMessageDropped();
            // 同上：与消息丢弃共用限流器。
            if (overloadLogLimiter_.allow())
            {
                LOG_WARN << "Room backlog full, dropped leave notice for room " << topic << ": "
                         << (1 + overloadLogLimiter_.takeSuppressed())
                         << " occurrence(s) since last log (total: ws_messages_dropped_total)";
            }
        }
        else
        {
            persistRoomMessage(*core_, topic, userName, msg_vo, roomSeq, "notice");
        }
    }
    catch (const std::exception& e)
    {
        Metrics::PrometheusRegistry::instance().recordWsHandlerException();
        LOG_ERROR << "Error in handleConnectionClosed: " << e.what();
    }
}

size_t ChatWebsocket::deliverDirectLocally(Core& core,
                                          const std::string& targetUser,
                                          const std::string& json,
                                          const WebSocketConnectionPtr& sender,
                                          bool* echoedToSender,
                                          size_t* backpressured)
{
    if (backpressured != nullptr)
    {
        *backpressured = 0;
    }

    // 先在锁内拷出目标会话列表，投递放在锁外 —— 不持锁做 IO。
    std::vector<Session> targets;
    {
        std::shared_lock lock(core.connMutex);
        if (const auto it = core.userNameToConn.find(targetUser); it != core.userNameToConn.end())
        {
            targets = it->second;
        }
    }

    // 真正可投递的份数（表里可能有空 conn 的残条目，那些不该占额度）
    size_t total = 0;
    for (const auto& s : targets)
    {
        if (s.conn)
        {
            ++total;
        }
    }
    if (total == 0)
    {
        return 0;
    }

    // ── 在途额度：先扣后派发，全有或全无 ──────────────────────────────────────
    //
    // 「全有或全无」而不是「有多少额度投多少」：后者会让同一个昵称的多端里
    // 一部分收到、一部分没收到，而调用方只能回一个 503 —— 客户端重试后
    // 已收到的那部分会看到重复消息。与 RoomRegistry 的多分片预检查同一取舍。
    //
    // 额度必须在【派发之前】扣减：反过来的话，「已入队但还没计数」的窗口里
    // 上游会以为下游还空着，正好在最该背压的时刻放行。
    auto inFlight = core.directInFlight;
    if (core.maxDirectInFlight != 0)
    {
        size_t cur = inFlight->load(std::memory_order_relaxed);
        for (;;)
        {
            if (cur + total > core.maxDirectInFlight)
            {
                if (backpressured != nullptr)
                {
                    *backpressured = total;
                }
                Metrics::PrometheusRegistry::instance().recordWsDirectBackpressured(total);
                return 0;
            }
            if (inFlight->compare_exchange_weak(cur, cur + total, std::memory_order_relaxed))
            {
                break;
            }
        }
    }
    else
    {
        // 限制关闭时仍然计数（gauge 才有意义），只是不检查上限
        inFlight->fetch_add(total, std::memory_order_relaxed);
    }

    // ── 按目标 loop 分组 ────────────────────────────────────────────────────
    //
    // 分组的意义有两层：
    //   ① 性能：同一昵称的 N 个多端会话散落在不同 loop 上时，逐份 send 就是
    //      N 次跨线程唤醒（queueInLoop + 一次系统调用）；分组后每个目标 loop
    //      只派发一次、携带整批连接，唤醒次数降为 O(loop 数)。与房间扇出的
    //      A1 优化同一手法。
    //   ② 正确性：只有在目标 loop 的线程里执行完这批 send，才能把在途额度还回去
    //      —— 裸 send 只是入队，立刻归还等于额度形同虚设。
    //
    // payload 用 shared_ptr 持有：派发出去的批次会活过本函数的栈帧（它要等
    // 目标 loop 执行到），形参 json 是引用，直接捕获就是悬垂引用。
    std::shared_ptr<const std::string> payload;
    std::vector<std::pair<TrantorLoopHandle, std::vector<WebSocketConnectionPtr>>> groups;
    groups.reserve(4);

    size_t dispatched = 0;
    bool echoed = false;
    for (const auto& s : targets)
    {
        if (!s.conn)
        {
            continue; // 空连接不占额度（total 里也没算它）
        }
        if (sender && s.conn == sender)
        {
            echoed = true;
        }

        const TrantorLoopHandle loop = s.sub ? s.sub->loop_ : TrantorLoopHandle{};
        if (!loop.valid())
        {
            // 取不到 loop（未登记 / 单测路径）：无处排队，退化为直投。
            // 语义与优化前一致，只是这一份不参与在途计数 —— 立刻归还额度。
            // ⚠️ 仍不判 connected()（跨线程读非原子成员是 data race）。
            s.conn->send(json);
            inFlight->fetch_sub(1, std::memory_order_relaxed);
            ++dispatched;
            continue;
        }

        auto it = std::find_if(groups.begin(), groups.end(),
                               [&loop](const auto& p) { return p.first == loop; });
        if (it == groups.end())
        {
            groups.emplace_back(loop, std::vector<WebSocketConnectionPtr>{});
            it = std::prev(groups.end());
        }
        it->second.push_back(s.conn);
    }

    if (!groups.empty())
    {
        payload = std::make_shared<const std::string>(json);
    }

    for (auto& [loop, conns] : groups)
    {
        const size_t batch = conns.size();
        auto batchConns = std::make_shared<const std::vector<WebSocketConnectionPtr>>(
            std::move(conns));
        try
        {
            loop.dispatch([payload, batchConns, inFlight, batch] {
                const std::string_view view(*payload);
                for (const auto& c : *batchConns)
                {
                    c->send(view);
                }
                inFlight->fetch_sub(batch, std::memory_order_relaxed);
            });
        }
        catch (...)
        {
            // 派发失败 ⇒ 这批永远不会被执行，必须立刻把额度还回去。
            // 否则在途计数只增不减，最终把整条私聊路径钉死在上限上。
            inFlight->fetch_sub(batch, std::memory_order_relaxed);
            throw;
        }
        dispatched += batch;
    }

    if (echoedToSender != nullptr)
    {
        *echoedToSender = echoed;
    }
    return dispatched;
}

bool ChatWebsocket::clusterBusEnabled()
{
    static const bool enabled = []() {
        try
        {
            const auto& custom = drogon::app().getCustomConfig();
            if (custom.isMember("enable_cluster_bus"))
            {
                return custom["enable_cluster_bus"].asBool();
            }
        }
        catch (...)
        {
        }
        // 缺省关闭：宁可静默单机运行，也不触发 drogon 的空条目退出崩溃（见头文件注释）
        return false;
    }();
    return enabled;
}

void ChatWebsocket::initClusterBus()
{
    // 延迟注册到框架启动事件中，确保 Redis 客户端已完全初始化并建立连接。
    //
    // 按值捕获 core_（shared_ptr）而不是 this：回调持有的引用会让 Core 活到回调结束，
    // 因此不依赖「控制器一定比定时器/回调活得久」这一脆弱的静态析构顺序假设。
    HttpAppFramework::instance().registerBeginningAdvice([core = core_] {
        try
        {
            if (!clusterBusEnabled())
            {
                LOG_DEBUG << "Redis Cluster Bus disabled (custom_config.enable_cluster_bus != true), "
                            "running in standalone mode.";
                return;
            }

            // 必须用 fast 变体：本工程 redis_clients 为 is_fast=true，
            // 且 redisUtils 全走 fast；取非 fast 变体会让 drogon 往
            // redisClientsMap_ 插入空条目，退出时必然段错误（见 clusterBusEnabled 注释）。
            auto redisClient = drogon::app().getFastRedisClient();
            if (!redisClient)
            {
                LOG_WARN << "Redis client is not configured, running in standalone mode.";
                return;
            }

            // 订阅分布式集群广播主题：通过 newSubscriber 获取长连接订阅者对象
            core->clusterSubscriber = redisClient->newSubscriber();
            core->clusterSubscriber->subscribe(
                "chat_cluster_bus",
                [core](const std::string& channel, const std::string& message) {
                    (void)channel;
                    try
                    {
                        ClusterPacket packet{};
                        // 容忍未知键：glaze 的默认是 error_on_unknown_keys = true，
                        // 于是「新版本实例发出的、带新字段的包」会被【旧版本】实例
                        // 直接丢弃 —— 滚动升级窗口里跨实例消息成片丢失，而现象只是
                        // 「另一个实例的客户端收不到消息」，极难定位。
                        // 本工程的消息总线协议还会继续演进（已经加过 toUser / roomSeq），
                        // 所以入站解析必须前向兼容：多出来的键忽略，不要当错误。
                        // ⚠️ 这只能让【本次之后】的新字段安全；已经发出去的旧二进制
                        //    改不了，滚动升级时仍需注意。
                        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(packet, message))
                        {
                            // 入站包解析失败原先完全静默（连日志都没有）。
                            // 其他实例的版本不兼容 / 载荷被截断时，本实例
                            // 表现为「莫名收不到消息」，没有任何可查的痕迹。
                            Metrics::PrometheusRegistry::instance().recordClusterPacketDropped();
                            static loong::log::RateLimiter badPacketLimiter{1000};
                            if (badPacketLimiter.allow())
                            {
                                LOG_WARN << "Cluster bus packet parse failed, dropped "
                                         << (1 + badPacketLimiter.takeSuppressed())
                                         << " occurrence(s) since last log";
                            }
                            return;
                        }

                        // 避免本实例回环消费自己刚发出的消息
                        if (packet.instId == core->instanceId)
                        {
                            return;
                        }

                        // ── 房间序检测（只报告，不改变投递行为）──────────────────
                        //
                        // 必须在真正投递【之前】做：投递可能抛异常，而「序号已经
                        // 收到了」是事实，不该因为下游投递失败而漏记。
                        //
                        // 基线键用 (instId, topic)：每个来源实例的房间序号是各自
                        // 独立单调的，跨来源之间没有可比性 —— 见 ClusterPacket::roomSeq
                        // 里关于能力边界的说明（这里【检测不到】跨来源乱序）。
                        if (packet.roomSeq != 0 && !packet.topic.empty())
                        {
                            checkClusterRoomSeq(*core, packet.instId, packet.topic, packet.roomSeq);
                        }

                        if (!packet.toUser.empty())
                        {
                            // 私聊：投给本实例上该用户名的所有在线会话。
                            // sender 传空 —— 消息来自其他实例，本实例的发送者不可能是它，
                            // 不存在「回显给自己」的问题；本实例没有这个昵称就自然投递 0 份。
                            deliverDirectLocally(*core, packet.toUser, packet.json, nullptr);
                            return;
                        }

                        // 房间广播：推给本实例房间内的所有客户端（同样分片并行扇出）
                        core->roomRegistry.publish(packet.topic, packet.json);
                    }
                    catch (const std::exception& e)
                    {
                        // 原先这里是空的 catch(...)：投递异常（分配失败等）
                        // 既无日志也无计数，属于纯盲区。
                        Metrics::PrometheusRegistry::instance().recordClusterPacketDropped();
                        Metrics::PrometheusRegistry::instance().recordWsHandlerException();
                        static loong::log::RateLimiter inboundErrLimiter{1000};
                        if (inboundErrLimiter.allow())
                        {
                            LOG_ERROR << "Cluster bus inbound handling failed: " << e.what()
                                      << " (" << (1 + inboundErrLimiter.takeSuppressed())
                                      << " occurrence(s) since last log)";
                        }
                    }
                });

            // 订阅调用是 noexcept 的（无错误回调），因此这里只能确认
            // 「newSubscriber + subscribe 都没抛」—— 这是框架能给的最强信号。
            // 置位后，私聊的 404 判定才会承认「跨实例可达」；
            // 未置位时宁可回 404 也不回假 200。
            core->clusterBusReady.store(true, std::memory_order_release);

            LOG_DEBUG << "Redis Cluster Bus initialized successfully, instanceId: " << core->instanceId;
        }
        catch (const std::exception& e)
        {
            // 保持 clusterBusReady == false：配置说开着但实际没起来时，
            // 必须让上层按「不可跨实例投递」处理，否则就是假 ACK。
            LOG_WARN << "Failed to initialize Redis Cluster Bus: " << e.what()
                     << " — cross-instance delivery is unavailable on this instance";
        }
    });
}

void ChatWebsocket::checkClusterRoomSeq(Core& core, const std::string& originInstId,
                                        const std::string& room, uint64_t roomSeq)
{
    if (roomSeq == 0)
    {
        return;
    }

    // 基线键：来源实例 + 房间。'\x1f'（单元分隔符）不会出现在实例 ID 或房间名里
    //（实例 ID 是 "inst_<pid>_<nanos>"，房间名来自 HTTP header/参数）——
    // 用不可能字符拼接，避免 ("a","b|c") 与 ("a|b","c") 撞同一个键。
    std::string key;
    key.reserve(originInstId.size() + room.size() + 1);
    key.append(originInstId).push_back('\x1f');
    key.append(room);

    uint64_t last = 0;
    {
        std::lock_guard lock(core.clusterSeqMtx);
        if (core.clusterLastSeq.size() >= kMaxClusterSeqBaselines &&
            !core.clusterLastSeq.contains(key))
        {
            // 见 kMaxClusterSeqBaselines 的说明：整体清空，宁可暂时不判，也不误报。
            core.clusterLastSeq.clear();
        }
        auto& slot = core.clusterLastSeq[key];
        last = slot;
        if (roomSeq > last)
        {
            slot = roomSeq;
        }
    }

    if (last == 0)
    {
        return; // 首次见到该来源：只建立基线，不做判断
    }

    if (roomSeq > last + 1)
    {
        // 缺口：中间有 (roomSeq - last - 1) 条从该来源发往该房间的消息没到达本实例。
        const uint64_t lost = roomSeq - last - 1;
        Metrics::PrometheusRegistry::instance().recordClusterSeqGap(lost);
        static loong::log::RateLimiter gapLimiter{1000};
        if (gapLimiter.allow())
        {
            LOG_WARN << "Cluster room seq gap from " << originInstId << " room " << room << ": saw "
                     << last << " then " << roomSeq << " (" << lost << " message(s) missing, "
                     << (1 + gapLimiter.takeSuppressed())
                     << " occurrence(s) since last log, total: ws_cluster_seq_gap_total)";
        }
        return;
    }

    if (roomSeq <= last)
    {
        // 回退：同一来源的消息乱序到达（或总线重复投递）。重复投递无害（客户端
        // 可凭 msg id 去重），乱序则会让两侧订阅者看到不同顺序 —— 都值得计数。
        Metrics::PrometheusRegistry::instance().recordClusterOutOfOrder();
        static loong::log::RateLimiter oooLimiter{1000};
        if (oooLimiter.allow())
        {
            LOG_WARN << "Cluster room seq regression from " << originInstId << " room " << room
                     << ": saw " << last << " then " << roomSeq << " ("
                     << (1 + oooLimiter.takeSuppressed())
                     << " occurrence(s) since last log, total: ws_cluster_out_of_order_total)";
        }
    }
}

void ChatWebsocket::publishToCluster(const Core& core, const std::string& topic,
                                     const std::string& json, const std::string& toUser,
                                     uint64_t roomSeq)
{
    // 未启用集群总线时直接返回：既省开销，也避免踩 drogon getRedisClient 的空条目坑
    if (!clusterBusEnabled())
    {
        return;
    }

    try
    {
        // fast 客户端按线程持有（IOThreadStorage），调用线程即其所属 loop 线程。
        //
        // ⚠️ 必须从 loop 线程调用：getFastRedisClient() 在非 loop 线程会越界访问
        //（IOThreadStorage 用 getCurrentThreadIndex() 索引，无 loop 时返回 SIZE_MAX），
        // 且 execCommandAsync 首行就 assertInLoopThread（失败会 LOG_FATAL + exit）。
        // 本函数的调用方（handleNewMessage / handleNewConnection / handleConnectionClosed）
        // 都在连接所属的 IO 线程内，满足该前提。
        auto redisClient = drogon::app().getFastRedisClient();
        if (!redisClient)
        {
            return;
        }

        ClusterPacket packet{
            .instId = core.instanceId,
            .topic = topic,
            .json = json,
            .toUser = toUser,
            .roomSeq = roomSeq
        };

        std::string payload{};
        (void)glz::write_json(packet, payload);

        // 极速异步发布至 Redis 广播频道 (非阻塞，零延迟开销)
        redisClient->execCommandAsync(
            [](const drogon::nosql::RedisResult&) {},
            [](const std::exception& e) {
                // ⚠️ 这条回调的触发频率与【消息速率】成正比（Redis 挂掉时每条都走）。
                // 原先直接 LOG_ERROR 无限流 —— 与已修过的「过载日志放大」是同一类
                // 问题，只是这次发生在集群路径上。计数不受日志轮转影响，
                // 是判断「跨实例投递是否在工作」的唯一可靠信号。
                Metrics::PrometheusRegistry::instance().recordClusterPublishFailed();
                static loong::log::RateLimiter publishErrLimiter{1000};
                if (publishErrLimiter.allow())
                {
                    LOG_ERROR << "Redis publish error: " << e.what() << " ("
                              << (1 + publishErrLimiter.takeSuppressed())
                              << " occurrence(s) since last log, total: ws_cluster_publish_failed_total)";
                }
            },
            "PUBLISH %s %s",
            "chat_cluster_bus",
            payload.c_str()
        );
    }
    catch (const std::exception& e)
    {
        // 这条路径在【每条消息】上都会走一遍，异常若持续发生，
        // 日志被限流/轮转掉之后就没有任何痕迹了，必须靠计数器留证。
        Metrics::PrometheusRegistry::instance().recordWsHandlerException();
        LOG_ERROR << "publishToCluster exception: " << e.what();
    }
}

void ChatWebsocket::checkAndEvictIdleConnections(Core& core)
{
    try
    {
        const auto now = Subscriber::nowNanos();
        // 60 秒无交互视为僵尸连接。
        //
        // 活动时间在【收到客户端任意帧】时刷新（Subscriber::touch()），包括两类：
        //   1. 业务文本消息（action == "message" / "ping" 等）；
        //   2. drogon 协议层自动收发的 Ping/Pong —— drogon 的 HttpServer 对每条
        //      WebSocket 连接默认执行 setPingMessage("", 30s)，即每 30 秒主动发一次
        //      Ping，浏览器/undici 等标准实现会自动回 Pong；客户端回的 Pong 会经
        //      WebSocketConnectionImpl 交给 handleNewMessage → touch()。
        // 因此「只收不发」的客户端【不会】被误杀；只有真正失联（不回 Pong）的连接
        // 才会在 60 秒后被驱逐。实测：不自动回 Pong 的裸客户端在空闲 64.8 秒时被踢。
        //
        // 注：服务端自己发的协议层 Ping 不会进 handleNewMessage（drogon 只在收到
        // 【对端】的 Ping/Pong 帧时才回调），所以协议层心跳不会替失联连接续期。
        //
        // chat.html 仍会额外发应用层心跳（action="ping"，25 秒一次），
        // 用于探测「协议栈还活着但前端 JS 已卡死」的场景，属于纵深防御而非必需。
        constexpr int64_t idleTimeoutNanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(60)).count();

        // 先在共享锁内做一次快照（连 Subscriber 一起拷出来），再在锁外判定与关闭。
        //
        // 快照带 Subscriber 是关键：判定完全不需要再碰 WebSocketConnection 的 context
        // —— drogon 的 connected()/hasContext()/getContextRef() 读的都是非原子成员，
        // 从主循环定时器跨线程调用属于 data race（形式上 UB）。Subscriber 里的
        // lastActiveNanos_ 是原子量，跨线程读安全。
        std::vector<Session> snapshot;
        {
            std::shared_lock lock(core.connMutex);
            snapshot.reserve(core.connCount.load(std::memory_order_relaxed));
            for (const auto& [name, sessions] : core.userNameToConn)
            {
                (void)name;
                snapshot.insert(snapshot.end(), sessions.begin(), sessions.end());
            }
        }

        for (const auto& s : snapshot)
        {
            if (!s.conn || !s.sub)
            {
                continue;
            }
            const auto idleNanos = now - s.sub->lastActiveNanos_.load(std::memory_order_relaxed);
            if (idleNanos > idleTimeoutNanos)
            {
                LOG_WARN << "Evicting idle connection: " << s.sub->userName_ << " (idle > 60s)";
                Metrics::PrometheusRegistry::instance().recordWsEvictedIdle();
                // forceClose 内部走 runInLoop 投递到连接所属 loop，跨线程调用安全；
                // 对已断开的连接是幂等的 no-op。
                s.conn->forceClose();
            }
        }
    }
    catch (const std::exception& e)
    {
        Metrics::PrometheusRegistry::instance().recordWsHandlerException();
        LOG_ERROR << "Error in checkAndEvictIdleConnections: " << e.what();
    }
}

void ChatWebsocket::publishRoomMetrics(Core& core)
{
    auto& m = Metrics::PrometheusRegistry::instance();
    const auto s = core.roomRegistry.stats();
    m.setRoomStats(s.rooms, s.shards, s.subscribers, s.maxRoomSubscribers, s.queuedMessages,
                   s.maxRoomBacklog);

    // ── 「最深积压房间」的边沿日志（C2）───────────────────────────────────────
    //
    // 房间名【不能】做成指标标签：它来自客户端 header，基数无界，是 Prometheus
    // 最经典的踩坑。所以数值进指标（ws_room_max_backlog），名字只能进日志。
    //
    // 两重防噪：
    //   ① 阈值 = 单分片上限的 1/4。低于它属于正常抖动 —— 一条消息从入队到被抽走
    //      之间队列总有一瞬间非空，逐次上报只会变成噪声。
    //   ② 只在「最深房间换了名字」时打。稳态下同一个房间长期最深 ⇒ 只打一行。
    //      这比「每 5 秒一行」和「每条丢弃一行」都克制，但仍能回答
    //      「到底是谁在积压」——这正是原先完全缺失的信息。
    const size_t cap = core.roomRegistry.options().backlogPerShard;
    const size_t warnAt = cap > 4 ? cap / 4 : 1;
    if (s.maxRoomBacklog >= warnAt)
    {
        if (s.worstRoom != core.lastWorstRoomLogged)
        {
            core.lastWorstRoomLogged = s.worstRoom;
            LOG_WARN << "Room backlog: deepest is '" << s.worstRoom << "' with " << s.maxRoomBacklog
                     << " queued message(s) (cap per shard: " << cap
                     << ", see ws_room_max_backlog / ws_room_queued_messages)";
        }
    }
    else
    {
        core.lastWorstRoomLogged.clear();
    }
    // 扇出分组效果（counter，看增量）：batches = 唤醒次数，deliveries = 实际份数
    m.setFanoutStats(core.roomRegistry.inLoopDeliveries(),
                     core.roomRegistry.crossThreadBatches(),
                     core.roomRegistry.crossThreadDeliveries());
    // 投递阶段异常计数（非 0 说明扇出过程中出过异常、当时那条消息未送达）
    m.setFanoutExceptions(core.roomRegistry.fanoutExceptions());
    // 全局在途投递份数（gauge）：唯一能看出「下游 loop 队列是否已堆满」的指标。
    // 配合 ws_fanout_inflight_limit 一起看 —— 只看当前值不知道离上限还有多远。
    // 持续贴近上限即说明消费端跟不上，此时 ws_messages_dropped_total 会开始上涨。
    m.setFanoutInflight(core.roomRegistry.inFlightDeliveries(),
                        core.roomRegistry.options().maxInFlightDeliveries,
                        core.roomRegistry.inflightRejectedCount());
}

void ChatWebsocket::publishKafkaMetrics()
{
    // 「拉」而不是「推」的理由见头文件：投递报告回调跑在 librdkafka 的 poll 线程上，
    // 在那种地方做额外工作会直接拖慢 poll。
    Metrics::PrometheusRegistry::instance().setKafkaDeliveryStats(
        kafka::KafkaManager::deliveryFailedCount(), kafka::KafkaManager::suppressedLogCount());
}

void ChatWebsocket::sweepDedupTable(Core& core)
{
    // 去重表的 TTL 清理。由 5 秒定时任务驱动。
    //
    // 为什么必须有 TTL 而不只是容量上限：
    //   ① 容量上限只在「写满」时才回收，稳态下内存停在高水位；
    //   ② 更要紧的是【错误用法的杀伤范围】。若客户端把 key 填成房间名，
    //      没有 TTL 的话它从第二条消息起会被永久静默丢弃 ——
    //      有 120 秒窗口的话，最坏也只是两分钟内丢消息，且能靠改客户端恢复。
    //
    // 但它是 O(表大小) 的，因此【先去重关闭时是空表】这条快速路径必须成立：
    // 表为空时循环立刻结束，不产生任何开销。
    //
    // ⚠️ 判空也必须在锁内：dedupSeen 是普通容器，锁外读 size/empty 就是 data race
    //    （IO 线程可能正在插入）。锁本身是零竞争的，不值得为省它引入 UB。
    const int64_t cutoff = loong::chat::nowMs() - Core::kDedupTtlMs;
    std::lock_guard lock(core.dedupMtx);
    if (core.dedupSeen.empty())
    {
        return;
    }
    for (auto it = core.dedupSeen.begin(); it != core.dedupSeen.end();)
    {
        if (it->second.atMs < cutoff)
        {
            it = core.dedupSeen.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void ChatWebsocket::installLatencySink(Core& core)
{
    // 注册表把「量到多少纳秒」交给这个回调，上报动作留在本层。
    //
    // 回调跑在【目标 IO loop】上（见 RoomRegistry::fanOutToSnapshot 里的说明），
    // 因此这里只允许做无锁、无分配、无 I/O 的事 —— recordFanoutLatencyNanos
    // 正好是纯原子操作。任何「写日志 / 发 HTTP / 加锁」都会直接拖慢事件循环，
    // 这在本工程是反复踩过的坑（librdkafka 的 deliveryReportCallback 就是反例）。
    core.roomRegistry.setLatencySink([](uint64_t nanos) {
        Metrics::PrometheusRegistry::instance().recordFanoutLatencyNanos(nanos);
    });
}
