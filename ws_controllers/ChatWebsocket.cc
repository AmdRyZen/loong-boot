#include "ChatWebsocket.h"
#include "utils/redisUtils.h"
#include "coroutinePool/TbbCoroutinePool.h"
#include "utils/PrometheusMetrics.h"
//#include "user.pb.h"
#include <glaze/glaze.hpp>
#include <drogon/HttpAppFramework.h>
#include "utils/retry_utils.h"
#include <memory_resource>
#include <atomic>
#include <vector>
#include <drogon/nosql/RedisSubscriber.h>

struct Subscriber
{
    std::string topic_;
    std::string userName_;
    RoomRegistry::SubscriberID id_{};

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

    std::atomic<int64_t> lastActiveNanos_{nowNanos()};
};

void ChatWebsocket::produceKafkaAsync(std::string topicName, std::string payload)
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

    if (!enableKafka)
    {
        return; // 开发或压测模式下跳过 Kafka 写入，保持极致 CPU 吞吐
    }

    const std::string topicForLog = topicName;

    // 背压：TBB 池积压超过上限时 submit 会返回 false。
    // 原实现忽略返回值 → 消息被静默丢弃，无日志无指标，上层误以为已落库。
    const bool accepted = TbbCoroutinePool::instance().submit(
        [topicName = std::move(topicName), payload = std::move(payload)] {
            rd_kafka_topic_t* topicPtr = kafka::KafkaManager::instance().getTopic(topicName);
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
        LOG_WARN << "TBB pool saturated, dropped Kafka persistence for topic: " << topicForLog;
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
            wsConn->send("pong_ms", WebSocketMessageType::Pong);
            LOG_INFO << "Received a ping";
            return;
        }

        if (type == WebSocketMessageType::Pong)
        {
            if (wsConn->hasContext())
            {
                auto& subscriber = wsConn->getContextRef<Subscriber>();
                subscriber.touch();
            }
            return;
        }

        if (type == WebSocketMessageType::Close)
        {
            LOG_INFO << "Received a Close";
            return;
        }

        if (!msg.empty())
        {
            chatMessageDto msg_dto{};
            if (glz::read_json(msg_dto, msg))
            {
                chatMessageVo err_msg{};
                std::string json{};
                (void)glz::write_json(err_msg, json);
                wsConn->send(json, WebSocketMessageType::Text);
                LOG_ERROR << "Failed to parse JSON message";
                return;
            }

            // hasContext() 必须判：连接在 handleNewConnection 中途失败时上下文可能未落，
            // getContextRef 会直接解引用空指针（UB），而不是抛异常。
            if (!wsConn->disconnected() && wsConn->hasContext())
            {
                auto& subscriber = wsConn->getContextRef<Subscriber>();
                subscriber.touch();
                const std::string& topic = subscriber.topic_;
                const std::string& senderName = subscriber.userName_;
                const auto id = subscriber.id_;

                if (!msg_dto.action.empty() && msg_dto.action == "message")
                {
                    Metrics::PrometheusRegistry::instance().recordWsMessage();

                    // 判断是否为点对点私聊 (toUser 非空)
                    if (!msg_dto.toUser.empty())
                    {
                        const std::string targetUser(msg_dto.toUser);

                        // 拷贝目标会话列表后在锁外发送，避免持锁做 IO
                        std::vector<WebSocketConnectionPtr> targets;
                        {
                            std::shared_lock lock(connMutex_);
                            if (const auto it = userNameToConn_.find(targetUser); it != userNameToConn_.end())
                            {
                                targets = it->second;
                            }
                        }

                        chatMessageVo msg_vo{};
                        msg_vo.code = 200;
                        msg_vo.id = id;
                        msg_vo.name = std::string_view(senderName);
                        msg_vo.message = std::format("[私聊] {}", std::string_view(msg_dto.msgContent));

                        std::string json{};
                        (void)glz::write_json(msg_vo, json);

                        // 投递给目标用户的所有在线会话（多端登录），发送者自己只回显一次
                        bool delivered = false;
                        bool echoedToSender = false;
                        for (const auto& conn : targets)
                        {
                            if (!conn || !conn->connected())
                            {
                                continue;
                            }
                            conn->send(json);
                            delivered = true;
                            if (conn == wsConn)
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

                        // [压测隔离] Kafka 推送已关闭（同上），避免压测时无消费者导致磁盘被写满。
                        // 生产环境持久化：私聊消息异步推至 Kafka 私聊主题
                        // produceKafkaAsync("chat_direct_topic", json);
                        return;
                    }

                    // 默认房间广播模式：直接把已序列化的 payload 交给 RoomRegistry。
                    // 保序由 RoomRegistry 内部的「房间级发布锁 + 分片 FIFO 队列」保证，
                    // 扇出本身在多核上并行，所以这里不再需要额外的串行派发器中转，
                    // 也不再经 TBB 池（多线程池会打乱入队顺序，下游再怎么串行都救不回来）。
                    chatMessageVo msg_vo{};
                    msg_vo.code = 200;
                    msg_vo.id = id;
                    msg_vo.name = std::string_view(senderName);
                    msg_vo.message = std::move(msg_dto.msgContent);

                    std::string json{};
                    (void)glz::write_json(msg_vo, json);

                    // 1. 本实例本地房间广播（分片并行扇出）
                    // 2. 分布式总线：同步广播给集群其他实例
                    // 3. 生产环境持久化：异步投递到 Kafka 历史消息流
                    if (!fanOutRoom(topic, json))
                    {
                        Metrics::PrometheusRegistry::instance().recordWsMessageDropped();
                        LOG_WARN << "Room backlog full, dropped message for room: " << topic;
                        sendOverloadNotice(wsConn);
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

        // 注册到房间注册表：分片直接持有连接指针，省掉 std::function 回调中转的开销
        subscriber->id_ = roomRegistry_.subscribe(topic, wsConn);

        // 同一昵称允许多端并存：注册为「昵称 -> 会话列表」，多端都能收到私聊。
        // 原实现用 emplace 存单连接，同名时静默失败，且断开时按昵称 erase 会误删新连接。
        {
            std::unique_lock lock(connMutex_);
            userNameToConn_[userName].push_back(wsConn);
        }

        LOG_INFO << "Added connection for user: " << userName << " Subscriber ID: " << subscriber->id_
                 << ", Topic: " << topic;

        chatMessageVo msg_vo;
        msg_vo.code = 200;
        msg_vo.id = subscriber->id_;
        msg_vo.name = topic;
        msg_vo.message = std::format("欢迎 {} 加入我们 {}", userName, topic);

        // 使用普通string避免thread_local问题
        std::string json{};
        (void)glz::write_json(msg_vo, json);

        // 与聊天消息共用同一房间的发布锁，保证「入群公告」与其他消息的相对顺序稳定
        if (!fanOutRoom(topic, json))
        {
            Metrics::PrometheusRegistry::instance().recordWsMessageDropped();
            LOG_WARN << "Room backlog full, dropped join notice for room: " << topic;
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "Error in handleNewConnection: " << e.what();
    }
}

void ChatWebsocket::handleConnectionClosed(const WebSocketConnectionPtr& wsConn)
{
    try
    {
        // 未登记成功的连接直接放过，避免 getContextRef 解引用空上下文（UB）
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
        {
            std::unique_lock lock(connMutex_);
            if (const auto it = userNameToConn_.find(userName); it != userNameToConn_.end())
            {
                auto& sessions = it->second;
                std::erase(sessions, wsConn);
                if (sessions.empty())
                {
                    userNameToConn_.erase(it);
                }
            }
        }
        LOG_INFO << "Removed user: " << userName;

        roomRegistry_.unsubscribe(topic, id);
        Metrics::PrometheusRegistry::instance().recordWsDisconnect();
        LOG_INFO << "Unsubscribed from topic: " << topic << ", ID: " << id;

        chatMessageVo msg_vo;
        msg_vo.code = 200;
        msg_vo.id = id;
        msg_vo.name = topic;
        msg_vo.message = std::format("{} 已离开 {}", userName, topic);

        // 使用普通string避免thread_local问题
        std::string json{};
        (void)glz::write_json(msg_vo, json);

        // 与聊天消息共用同一房间的发布锁，保证「离群公告」与其他消息的相对顺序稳定
        if (!fanOutRoom(topic, json))
        {
            Metrics::PrometheusRegistry::instance().recordWsMessageDropped();
            LOG_WARN << "Room backlog full, dropped leave notice for room: " << topic;
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "Error in handleConnectionClosed: " << e.what();
    }
}

void ChatWebsocket::initClusterBus()
{
    // 延迟注册到框架启动事件中，确保 Redis 客户端已完全初始化并建立连接
    HttpAppFramework::instance().registerBeginningAdvice([this]() {
        try
        {
            auto redisClient = drogon::app().getRedisClient();
            if (!redisClient)
            {
                LOG_WARN << "Redis client is not configured, running in standalone mode.";
                return;
            }

            // 订阅分布式集群广播主题：通过 newSubscriber 获取长连接订阅者对象
            clusterSubscriber_ = redisClient->newSubscriber();
            clusterSubscriber_->subscribe(
                "chat_cluster_bus",
                [this](const std::string& channel, const std::string& message) {
                    try
                    {
                        ClusterPacket packet{};
                        if (glz::read_json(packet, message))
                        {
                            return;
                        }

                        // 避免本实例回环消费自己刚发出的消息
                        if (packet.instId == instanceId_)
                        {
                            return;
                        }

                        // 收到来自其他实例的广播，推给本实例房间内的所有客户端（同样分片并行扇出）
                        roomRegistry_.publish(packet.topic, packet.json);
                    }
                    catch (...)
                    {
                    }
                });

            LOG_INFO << "Redis Cluster Bus initialized successfully, instanceId: " << instanceId_;
        }
        catch (const std::exception& e)
        {
            LOG_WARN << "Failed to initialize Redis Cluster Bus: " << e.what();
        }
    });
}

void ChatWebsocket::publishToCluster(const std::string& topic, const std::string& json) const
{
    try
    {
        auto redisClient = drogon::app().getRedisClient();
        if (!redisClient)
        {
            return;
        }

        ClusterPacket packet{
            .instId = instanceId_,
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

void ChatWebsocket::checkAndEvictIdleConnections()
{
    try
    {
        const auto now = Subscriber::nowNanos();
        // 60 秒无心跳/无交互视为僵尸连接
        constexpr int64_t idleTimeoutNanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(60)).count();

        // 先在共享锁内做一次快照，再在锁外做 connected()/forceClose()，
        // 避免长时间独占锁阻塞 IO 线程的连接注册与注销。
        std::vector<std::pair<WebSocketConnectionPtr, std::string>> snapshot;
        {
            std::shared_lock lock(connMutex_);
            snapshot.reserve(userNameToConn_.size());
            for (const auto& [name, sessions] : userNameToConn_)
            {
                for (const auto& conn : sessions)
                {
                    snapshot.emplace_back(conn, name);
                }
            }
        }

        for (const auto& [conn, name] : snapshot)
        {
            if (!conn)
            {
                continue;
            }
            if (!conn->connected())
            {
                conn->forceClose();
                continue;
            }
            if (conn->hasContext())
            {
                const auto& sub = conn->getContextRef<Subscriber>();
                const auto idleNanos = now - sub.lastActiveNanos_.load(std::memory_order_relaxed);
                if (idleNanos > idleTimeoutNanos)
                {
                    LOG_WARN << "Evicting idle connection: " << name << " (idle > 60s)";
                    conn->forceClose(); // 主动切断死连接
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "Error in checkAndEvictIdleConnections: " << e.what();
    }
}
