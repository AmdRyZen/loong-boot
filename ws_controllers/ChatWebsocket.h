#pragma once
#include <drogon/WebSocketController.h>
#include "kafka/KafkaManager.h"
#include <glaze/glaze.hpp>
#include <drogon/HttpAppFramework.h>
#include "utils/retry_utils.h"
#include "parallel_hashmap/phmap.h"
#include "RoomRegistry.h"
#include <algorithm>
#include <cstdlib>
#include <memory_resource>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <drogon/nosql/RedisClient.h>
#include <random>
#include <chrono>

using namespace drogon;

class ChatWebsocket final : public WebSocketController<ChatWebsocket>
{
public:
    ChatWebsocket() : roomRegistry_(makeFanoutOptions())
    {
        constexpr size_t estimatedUserCount = 10000; // 可用配置替代硬编码
        userNameToConn_.reserve(estimatedUserCount);

        // 生成当前服务实例唯一的 Instance ID (防止集群跨机广播回环)
        std::mt19937_64 rng(std::random_device{}());
        instanceId_ = "inst_" + std::to_string(rng());

        // 注册定时任务：空闲超时连接驱逐 + 房间指标采集
        HttpAppFramework::instance().getLoop()->runEvery(5.0, [this] {
            checkAndEvictIdleConnections();
            publishRoomMetrics();
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
    // 房间注册表：按订阅者分片 + 多核并行扇出，严格保序。
    // 取代了原先的 drogon::PubSubService（它单线程扇出，房间越大越慢）
    // 与 RoomSerialDispatcher（保序职责已内聚到 RoomRegistry 的房间级发布锁）。
    RoomRegistry roomRegistry_;

    // 用户名 -> 该用户名下的所有在线会话（同一昵称允许多端并存，多端都能收到私聊）
    // 原先的 name -> 单连接 语义在「同名多连接」时会串号：旧连接断开时会把新连接的
    // 记录一并 erase 掉，导致新用户从此收不到私聊。
    phmap::parallel_flat_hash_map<std::string, std::vector<WebSocketConnectionPtr>> userNameToConn_;

    // 保护 userNameToConn_ 的复合操作与遍历。
    // 读路径（私聊查表 / 定时任务遍历）走共享锁，连接注册与注销走独占锁，
    // 彻底消除原实现「无锁遍历 vs IO 线程 emplace/erase」的数据竞争。
    mutable std::shared_mutex connMutex_;

    std::string instanceId_;
    std::shared_ptr<drogon::nosql::RedisSubscriber> clusterSubscriber_;
    void initClusterBus();
    void publishToCluster(const std::string& topic, const std::string& json) const;
    static void produceKafkaAsync(std::string topicName, std::string payload);

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
    static bool clusterBusEnabled();

    struct ClusterPacket
    {
        std::string instId;
        std::string topic;
        std::string json;
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
        return opt;
    }

    void checkAndEvictIdleConnections();

    // 把 RoomRegistry 的房间侧快照与扇出分组计数推送到 Prometheus registry。
    // 由 5 秒定时任务驱动：/metrics 抓取时就不必再去加房间表的锁，
    // 代价是最多 5 秒的滞后（gauge 类指标可以接受；counter 看增量也不受影响）。
    void publishRoomMetrics();

    // 把 room 内的消息投递到本地房间 + 集群总线（+ Kafka 持久化，已按需关闭）
    // 返回 false 表示本地分片积压达上限、消息被丢弃（调用方需计数并告知客户端）
    bool fanOutRoom(const std::string& topic, const std::string& json)
    {
        const bool ok = roomRegistry_.publish(topic, json);
        publishToCluster(topic, json);
        // [压测隔离] Kafka 推送已关闭：压测只生产不消费会把磁盘写满。
        // 恢复：取消下行注释，并确认 custom_config.enable_kafka_persistence 为 true。
        // produceKafkaAsync("chat_messages_topic", json);
        return ok;
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
