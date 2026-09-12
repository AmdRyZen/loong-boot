#include "ChatWebsocket.h"
#include "utils/redisUtils.h"
#include "coroutinePool/TbbCoroutinePool.h"
#include "utils/PrometheusMetrics.h"
//#include "user.pb.h"
#include <glaze/glaze.hpp>
#include <drogon/HttpAppFramework.h>
#include "utils/retry_utils.h"
#include <atomic>
#include <vector>
#include <drogon/nosql/RedisSubscriber.h>

// 注：Subscriber / Session / Core 的定义都在 ChatWebsocket.h 里。
// Subscriber 原先定义在本文件，为了让定时器与启动回调能够按值捕获 Core
//（而不是捕获 this）而整体上移到头文件。

void ChatWebsocket::produceKafkaAsync(std::string_view topicName, std::string_view payload)
{
    // 读取生产环境配置开关：通过 enable_kafka_persistence 控制是否异步落库 Kafka
    static const bool enableKafka = []() {
        try
        {
            const auto& custom = drogon::app().getCustomConfig();
            if (custom.isMember("enable_kafka_persistence"))
            {
                return custom["enable_kafka_persistence"].asBool();
            }
        }
        catch (...)
        {
        }
        return true;
    }();

    // 开关检查必须在【任何字符串构造之前】。
    //
    // 形参原先按值传 std::string：即便 Kafka 关闭、函数在这里立刻 return，
    // 实参的堆拷贝也已经发生完了 —— 热路径上每条消息白付一次分配。
    // 改成 string_view 后，关闭态下这次调用只是两次指针/长度赋值。
    if (!enableKafka)
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
            rd_kafka_topic_t* topicPtr = kafka::KafkaManager::instance().getTopic(topic);
            retryWithSleep([&]() {
                if (!kafka::KafkaManager::safeProduce(topicPtr, payload))
                {
                    const rd_kafka_resp_err_t err = rd_kafka_last_error();
                    LOG_ERROR << "Failed to produce message: " << rd_kafka_err2str(err);
                    return err != RD_KAFKA_RESP_ERR__QUEUE_FULL;
                }
                return true;
            });
        });

    if (!accepted)
    {
        Metrics::PrometheusRegistry::instance().recordWsMessageDropped();
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

                        // 拷贝目标会话列表后在锁外发送，避免持锁做 IO
                        std::vector<Session> targets;
                        {
                            std::shared_lock lock(core_->connMutex);
                            if (const auto it = core_->userNameToConn.find(targetUser);
                                it != core_->userNameToConn.end())
                            {
                                targets = it->second;
                            }
                        }

                        chatMessageVo msg_vo{};
                        msg_vo.code = 200;
                        msg_vo.id = nextMessageId();
                        msg_vo.name = std::string_view(senderName);
                        msg_vo.message = std::format("[私聊] {}", std::string_view(msg_dto.msgContent));

                        std::string json{};
                        (void)glz::write_json(msg_vo, json);

                        // 投递给目标用户的所有在线会话（多端登录），发送者自己只回显一次
                        bool delivered = false;
                        bool echoedToSender = false;
                        for (const auto& s : targets)
                        {
                            if (!s.conn || !s.conn->connected())
                            {
                                continue;
                            }
                            s.conn->send(json);
                            delivered = true;
                            if (s.conn == wsConn)
                            {
                                echoedToSender = true;
                            }
                        }

                        if (!delivered)
                        {
                            // 目标离线提示
                            wsConn->send(buildNoticeJson(
                                404, "系统通知", std::format("用户 {} 当前不在线", targetUser).c_str()));
                            return;
                        }

                        if (!echoedToSender)
                        {
                            wsConn->send(json);
                        }

                        // 生产环境持久化：私聊消息异步推至 Kafka 私聊主题。
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
        LOG_ERROR << "Error in handleConnectionClosed: " << e.what();
    }
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

                        // 收到来自其他实例的广播，推给本实例房间内的所有客户端（同样分片并行扇出）
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

void ChatWebsocket::publishToCluster(const Core& core, const std::string& topic, const std::string& json)
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
            .json = json
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
