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
    bool kafka = false;
    try
    {
        Json::Value root;
        Json::CharReaderBuilder builder;
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        std::string errs;
        std::ifstream ifs(Config::filePath(), std::ios::binary);
        const bool parsed =
            ifs && Json::parseFromStream(builder, ifs, &root, &errs);
        if (parsed)
        {
            const auto& custom = root["custom_config"];
            if (custom.isMember("enable_kafka_persistence"))
            {
                kafka = custom["enable_kafka_persistence"].asBool();
            }
        }
        else
        {
            // 读不到就保持「关闭」并只告警一次。
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
    Metrics::PrometheusRegistry::instance().setSwitchStates(kafka);
}

void ChatWebsocket::produceKafkaAsync(std::string_view topicName, std::string_view payload)
{
    // 开关检查必须在【任何字符串构造之前】。
    //
    // 形参原先按值传 std::string：即便 Kafka 关闭、函数在这里立刻 return，
    // 实参的堆拷贝也已经发生完了 —— 热路径上每条消息白付一次分配。
    // 改成 string_view 后，关闭态下这次调用只是两次指针/长度赋值。
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
        [topic = std::string(topicName), payload = std::string(payload)] {
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

            // delivered 必须显式追踪：谓词的返回值语义是「还要不要重试」而不是「成功没有」
            //（QUEUE_FULL 之外的错误返回 true 表示「重试没意义、放弃」，会被 retryWithSleep
            // 当作成功收尾）。原实现里「重试耗尽」与「永久性错误放弃」都既无指标也无终态日志，
            // 上层只能靠翻每次尝试的 LOG_ERROR 自己拼结论。
            //
            // ⚠️ 循环内【一行日志都不打】：retryWithSleep 默认重试 3 次，若每次失败都打一行，
            // broker 挂掉时就是「每条消息 3 行」——正是本项目反复踩过的热路径日志放大，
            // 而且它比终态那条还频繁（终态已限流，这条没有）。改成把错误码记在 lastErr 里、
            // 由终态那条限流日志输出：信息量不减（拿到的是最终错误码，比三次中间态更有用），
            // 日志量从 O(3 × 消息数) 降到 ≤1 行/秒。
            bool delivered = false;
            rd_kafka_resp_err_t lastErr = RD_KAFKA_RESP_ERR_NO_ERROR;
            retryWithSleep([&]() {
                if (kafka::KafkaManager::safeProduce(topicPtr, payload))
                {
                    delivered = true;
                    return true;
                }
                lastErr = rd_kafka_last_error();
                // QUEUE_FULL 是瞬态的 → 返回 false 让 retryWithSleep 重试；
                // 其余错误（消息过大 / 无效 topic 等）重试无意义 → 放弃。
                return lastErr != RD_KAFKA_RESP_ERR__QUEUE_FULL;
            });
            if (delivered)
            {
                return;
            }

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

                if (!msg_dto.action.empty() && msg_dto.action == "message")
                {
                    Metrics::PrometheusRegistry::instance().recordWsMessage();

                    // 判断是否为点对点私聊 (toUser 非空)
                    if (!msg_dto.toUser.empty())
                    {
                        const std::string targetUser(msg_dto.toUser);

                        chatMessageVo msg_vo{};
                        msg_vo.code = 200;
                        msg_vo.id = nextMessageId();
                        msg_vo.name = std::string_view(senderName);
                        msg_vo.message = std::format("[私聊] {}", std::string_view(msg_dto.msgContent));

                        std::string json{};
                        (void)glz::write_json(msg_vo, json);

                        // ① 本实例本地投递（多端登录：该昵称的所有在线会话都收到）
                        bool echoedToSender = false;
                        const size_t localDelivered =
                            deliverDirectLocally(*core_, targetUser, json, wsConn, &echoedToSender);

                        // 本实例查无此人，且没开集群总线 → 可以确定不在线，明确告知。
                        // 开了总线就不能这么断言了：对方可能在别的实例上，同步无法确认，
                        // 只能投出去让总线去找（宁可静默，也不能误报「不在线」）。
                        if (localDelivered == 0 && !clusterBusEnabled())
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
                        produceKafkaAsync("chat_direct_topic", json);
                        return;
                    }

                    // 默认房间广播模式：直接把已序列化的 payload 交给 RoomRegistry。
                    // 保序由 RoomRegistry 内部的「房间级发布锁 + 分片 FIFO 队列」保证，
                    // 扇出本身在多核上并行，所以这里不再需要额外的串行派发器中转，
                    // 也不再经 TBB 池（多线程池会打乱入队顺序，下游再怎么串行都救不回来）。
                    chatMessageVo msg_vo{};
                    msg_vo.code = 200;
                    msg_vo.id = nextMessageId();
                    msg_vo.name = std::string_view(senderName);
                    msg_vo.message = std::move(msg_dto.msgContent);

                    std::string json{};
                    (void)glz::write_json(msg_vo, json);

                    // 1. 本实例本地房间广播（分片并行扇出）
                    // 2. 分布式总线：同步广播给集群其他实例
                    // 3. 生产环境持久化：异步投递到 Kafka 历史消息流
                    // 带上本连接缓存的房间句柄：hint 与 topic 同源（都是本订阅者的字段），
                    // 因此 publish 可以安全地跳过全局房间表锁。
                    if (!fanOutRoom(*core_, topic, json, /*broadcastToCluster=*/true,
                                    subscriber.room_))
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
                            LOG_WARN << "Room backlog full, message dropped: "
                                     << (1 + overloadLogLimiter_.takeSuppressed())
                                     << " occurrence(s) since last log "
                                     << "(total: ws_messages_dropped_total)";
                        }
                    }
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
        subscriber->id_ = core_->roomRegistry.subscribe(
            topic, wsConn, TrantorLoopHandle{trantor::EventLoop::getEventLoopOfCurrentThread()});

        // 缓存房间句柄：此后这条连接的每次发布都带上传回的句柄，publish 走快路径
        // 直接命中该房间，不再每条消息都去抢全局 roomsMtx_ 共享锁。
        // 房间被回收后句柄自动失效（retired），publish 会回退查表，无需在退订时清理。
        subscriber->room_ = core_->roomRegistry.acquireRoomHandle(topic);

        // 同一昵称允许多端并存：注册为「昵称 -> 会话列表」，多端都能收到私聊。
        // 原实现用 emplace 存单连接，同名时静默失败，且断开时按昵称 erase 会误删新连接。
        // 同时把 Subscriber 一并存下来，供空闲驱逐在锁内直接取用（避免跨线程读连接的 context）。
        {
            std::unique_lock lock(core_->connMutex);
            core_->userNameToConn[userName].push_back(Session{wsConn, subscriber});
            core_->connCount.fetch_add(1, std::memory_order_relaxed);
        }

        // 每连接一条的诊断信息，不是「有问题」的信号 → DEBUG
        LOG_DEBUG << "Added connection for user: " << userName << " Subscriber ID: " << subscriber->id_
                 << ", Topic: " << topic;

        chatMessageVo msg_vo;
        msg_vo.code = 200;
        msg_vo.id = nextMessageId();
        msg_vo.name = topic;
        msg_vo.message = std::format("欢迎 {} 加入我们 {}", userName, topic);

        // 使用普通string避免thread_local问题
        std::string json{};
        (void)glz::write_json(msg_vo, json);

        // 与聊天消息共用同一房间的发布锁，保证「入群公告」与其他消息的相对顺序稳定。
        //
        // broadcastToCluster = false：在线状态是本实例的本地事实。广播给其他实例后，
        // 那边的同名用户会收到与自己无关的「XX 已加入」；退群公告同理（见下）。
        if (!fanOutRoom(*core_, topic, json, /*broadcastToCluster=*/false, subscriber->room_))
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

        // 使用普通string避免thread_local问题
        std::string json{};
        (void)glz::write_json(msg_vo, json);

        // 与聊天消息共用同一房间的发布锁，保证「离群公告」与其他消息的相对顺序稳定。
        // 同样不跨实例广播（理由见 handleNewConnection 的入群公告）。
        if (!fanOutRoom(*core_, topic, json, /*broadcastToCluster=*/false, subscriber.room_))
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
                                          bool* echoedToSender)
{
    // 先在锁内拷出目标会话列表，投递放在锁外 —— 不持锁做 IO。
    std::vector<Session> targets;
    {
        std::shared_lock lock(core.connMutex);
        if (const auto it = core.userNameToConn.find(targetUser); it != core.userNameToConn.end())
        {
            targets = it->second;
        }
    }

    size_t delivered = 0;
    bool echoed = false;
    for (const auto& s : targets)
    {
        if (!s.conn)
        {
            continue;
        }
        // ⚠️ 刻意不判 s.conn->connected()：那是非原子成员，而本函数可能运行在
        // Redis 订阅回调线程上（跨实例私聊），目标连接却属于另一个 IO 线程 ——
        // 跨线程读它就是 data race（形式上 UB，且会把整条集群路径变成随机炸弹）。
        // 已断开的连接上调用 send 是安全的：drogon 在 sendInLoop 里静默丢弃。
        // 「表里有」≈「在线」也成立：条目在 handleConnectionClosed 里摘除。
        s.conn->send(json);
        ++delivered;
        if (sender && s.conn == sender)
        {
            echoed = true;
        }
    }

    if (echoedToSender != nullptr)
    {
        *echoedToSender = echoed;
    }
    return delivered;
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
                    try
                    {
                        ClusterPacket packet{};
                        if (glz::read_json(packet, message))
                        {
                            return;
                        }

                        // 避免本实例回环消费自己刚发出的消息
                        if (packet.instId == core->instanceId)
                        {
                            return;
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
                    catch (...)
                    {
                    }
                });

            LOG_DEBUG << "Redis Cluster Bus initialized successfully, instanceId: " << core->instanceId;
        }
        catch (const std::exception& e)
        {
            LOG_WARN << "Failed to initialize Redis Cluster Bus: " << e.what();
        }
    });
}

void ChatWebsocket::publishToCluster(const Core& core, const std::string& topic,
                                     const std::string& json, const std::string& toUser)
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
            .toUser = toUser
        };

        std::string payload{};
        (void)glz::write_json(packet, payload);

        // 极速异步发布至 Redis 广播频道 (非阻塞，零延迟开销)
        redisClient->execCommandAsync(
            [](const drogon::nosql::RedisResult&) {},
            [](const std::exception& e) {
                LOG_ERROR << "Redis publish error: " << e.what();
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

void ChatWebsocket::publishRoomMetrics(const Core& core)
{
    auto& m = Metrics::PrometheusRegistry::instance();
    const auto s = core.roomRegistry.stats();
    m.setRoomStats(s.rooms, s.shards, s.subscribers, s.maxRoomSubscribers);
    // 扇出分组效果（counter，看增量）：batches = 唤醒次数，deliveries = 实际份数
    m.setFanoutStats(core.roomRegistry.inLoopDeliveries(),
                     core.roomRegistry.crossThreadBatches(),
                     core.roomRegistry.crossThreadDeliveries());
}
