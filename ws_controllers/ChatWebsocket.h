#pragma once
#include <drogon/PubSubService.h>
#include <drogon/WebSocketController.h>
#include "kafka/KafkaManager.h"
#include <glaze/glaze.hpp>
#include <drogon/HttpAppFramework.h>
#include "utils/retry_utils.h"
#include "parallel_hashmap/phmap.h"
#include "RoomSerialDispatcher.h"
#include <memory_resource>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include <drogon/nosql/RedisClient.h>
#include <random>
#include <chrono>

using namespace drogon;

class ChatWebsocket final : public WebSocketController<ChatWebsocket>
{
public:
    ChatWebsocket()
    {
        constexpr size_t estimatedUserCount = 10000; // 可用配置替代硬编码
        userNameToConn_.reserve(estimatedUserCount);

        // 生成当前服务实例唯一的 Instance ID (防止集群跨机广播回环)
        std::mt19937_64 rng(std::random_device{}());
        instanceId_ = "inst_" + std::to_string(rng());

        // 注册定时任务：心跳探测与空闲超时连接驱逐
        HttpAppFramework::instance().getLoop()->runEvery(5.0, [this] {
            checkAndEvictIdleConnections();
            sendHeartbeatToAll();
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
    WS_PATH_ADD("/chat");
    WS_ADD_PATH_VIA_REGEX("/[^/]*", Get);
    WS_PATH_LIST_END

private:
    PubSubService<std::string> chatRooms_;

    // 用户名 -> 该用户名下的所有在线会话（同一昵称允许多端并存，多端都能收到私聊）
    // 原先的 name -> 单连接 语义在「同名多连接」时会串号：旧连接断开时会把新连接的
    // 记录一并 erase 掉，导致新用户从此收不到私聊。
    phmap::parallel_flat_hash_map<std::string, std::vector<WebSocketConnectionPtr>> userNameToConn_;

    phmap::flat_hash_set<std::string> excludedUsers_ = {"dog", "cat", "mouse"};

    // 保护 userNameToConn_ 的复合操作与遍历。
    // 读路径（私聊查表 / 定时任务遍历）走共享锁，连接注册与注销走独占锁，
    // 彻底消除原实现「无锁遍历 vs IO 线程 emplace/erase」的数据竞争。
    mutable std::shared_mutex connMutex_;

    // 房间级串行派发器：保证同一房间消息严格 FIFO，不同房间并行不互斥
    RoomSerialDispatcher roomDispatcher_;

    std::string instanceId_;
    std::shared_ptr<drogon::nosql::RedisSubscriber> clusterSubscriber_;
    void initClusterBus();
    void publishToCluster(const std::string& topic, const std::string& json) const;
    static void produceKafkaAsync(std::string topicName, std::string payload);

    struct ClusterPacket
    {
        std::string instId;
        std::string topic;
        std::string json;
    };

    void checkAndEvictIdleConnections();

    // 把 room 内的消息投递到本地房间 + 集群总线（+ Kafka 持久化，已按需关闭）
    void fanOutRoom(const std::string& topic, const std::string& json)
    {
        chatRooms_.publish(topic, json);
        publishToCluster(topic, json);
        // [压测隔离] Kafka 推送已关闭：压测只生产不消费会把磁盘写满。
        // 恢复：取消下行注释，并确认 custom_config.enable_kafka_persistence 为 true。
        // produceKafkaAsync("chat_messages_topic", json);
    }

    void sendHeartbeatToAll() const
    {
        // 先加共享锁做一次快照，再在锁外发送。
        // 原实现无锁遍历 userNameToConn_，与 IO 线程的注册/注销并发修改容器 = UB。
        std::vector<std::pair<std::string, WebSocketConnectionPtr>> snapshot;
        {
            std::shared_lock lock(connMutex_);
            snapshot.reserve(userNameToConn_.size());
            for (const auto& [name, sessions] : userNameToConn_)
            {
                for (const auto& conn : sessions)
                {
                    snapshot.emplace_back(name, conn);
                }
            }
        }

        chatRooms_.publish("001", std::format("房间公告消息"));

        // 遍历并发送心跳给 excludedUsers_ 内的用户
        for (const auto& [userName, wsConnPtr] : snapshot)
        {
            // 跳过未连接或不在排除列表的用户
            if (!wsConnPtr || !wsConnPtr->connected() || !excludedUsers_.contains(userName))
                continue;

            // monotonic_buffer_resource 不会自动回收，每轮显式 release 防止长期运行内存单调增长
            thread_local std::pmr::monotonic_buffer_resource pool(1024 * 1024);
            pool.release();

            chatMessageVo messageVo{
                .code = 200,
                .id = 0,
                .name = std::pmr::string(userName, &pool),
                .message = std::pmr::string(std::format("{} 心跳检测 正常 这是定制消息", userName), &pool)
            };

            std::pmr::string json(&pool);
            (void)glz::write_json(messageVo, json);

            // 发送给客户端
            wsConnPtr->send(json);

            // [压测隔离] Kafka 推送已关闭（同上），避免压测时无消费者导致磁盘被写满。
            // produceKafkaAsync("message_topic_one", std::string(json.data(), json.size()));
        }
    }

    struct chatMessageDto
    {
        std::pmr::string key;
        std::pmr::string action;
        std::pmr::string msgContent;
        std::pmr::string toUser;  // 点对点私聊目标用户名 (为空表示房间广播)
    };

    struct chatMessageVo
    {
        int code = -1;
        uint64_t id = 0;
        std::pmr::string name;
        std::pmr::string message;
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

    // 过载时明确告知客户端消息未投递，避免「静默丢消息」让上层误以为已送达
    static void sendOverloadNotice(const WebSocketConnectionPtr& conn)
    {
        if (conn && conn->connected())
        {
            conn->send(buildNoticeJson(503, "系统通知", "服务器繁忙，消息未投递，请稍后重试"));
        }
    }
};
