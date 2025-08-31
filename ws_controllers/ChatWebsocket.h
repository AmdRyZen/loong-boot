#pragma once
#include <drogon/PubSubService.h>
#include <drogon/WebSocketController.h>
#include "kafka/KafkaManager.h"
#include <glaze/glaze.hpp>
#include <drogon/HttpAppFramework.h>
#include "utils/retry_utils.h"
#include "parallel_hashmap/phmap.h"

using namespace drogon;

class ChatWebsocket final : public WebSocketController<ChatWebsocket>
{
public:
    ChatWebsocket()
    {
        constexpr size_t estimatedUserCount = 10000; // 可用配置替代硬编码
        connToUser_.reserve(estimatedUserCount);
        userNameToConn_.reserve(estimatedUserCount);

        // 使用 drogon::HttpAppFramework::instance()
        HttpAppFramework::instance().getLoop()->runEvery(10.0, [this] {
            sendHeartbeatToAll();
        });
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
    //std::unordered_set<WebSocketConnectionPtr> connections_;
    phmap::parallel_flat_hash_map<std::string, WebSocketConnectionPtr> userNameToConn_;
    phmap::parallel_flat_hash_map<WebSocketConnectionPtr, std::string> connToUser_;
    phmap::flat_hash_set<std::string> excludedUsers_ = {"dog", "cat", "mouse"};
    mutable std::mutex mutex_;

    void sendHeartbeatToAll() const
    {
        rd_kafka_topic_t* topic_ptr = kafka::KafkaManager::instance().getTopic("message_topic_one");
        chatRooms_.publish("001", std::format("房间公告消息"));

        // 遍历并发送心跳给 excludedUsers_ 内的用户
        for (const auto& [userName, wsConnPtr] : userNameToConn_)
        {
            // 跳过未连接或不在排除列表的用户
            if (!wsConnPtr->connected() || !excludedUsers_.contains(userName))
                continue;

            chatMessageVo messageVo{};
            messageVo.code = 200;
            messageVo.id = 0;
            messageVo.name = userName;
            messageVo.message = std::format("{} 心跳检测 正常 这是定制消息", userName);

            thread_local std::string buffer;
            buffer.clear();
            (void)glz::write_json(messageVo, buffer);

            // 发送给客户端
            wsConnPtr->send(buffer);

            // Kafka 异步发送，避免阻塞心跳循环
            auto kafkaTask = [topic_ptr]() {
                retryWithSleep([&]() {
                    if (!kafka::KafkaManager::safeProduce(topic_ptr, buffer))
                    {
                        const rd_kafka_resp_err_t err = rd_kafka_last_error();
                        LOG_ERROR << "Failed to produce message: " << rd_kafka_err2str(err);
                        return err != RD_KAFKA_RESP_ERR__QUEUE_FULL;
                    }
                    return true;
                });
            };

            // 使用 drogon 的线程池执行 Kafka 发送
            HttpAppFramework::instance().getLoop()->runInLoop(kafkaTask);
        }
    }

    struct chatMessageDto
    {
        std::string key;
        std::string action;
        std::string msgContent;
    };

    struct chatMessageVo
    {
        int code = -1;
        uint64_t id;
        std::string name;
        std::string message;
    };
};