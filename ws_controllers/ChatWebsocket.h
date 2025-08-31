#pragma once
#include <drogon/PubSubService.h>
#include <drogon/WebSocketController.h>
#include "kafka/KafkaManager.h"
#include <glaze/glaze.hpp>
#include <drogon/HttpAppFramework.h>
#include "utils/retry_utils.h"
#include <boost/unordered_map.hpp>
#include <boost/unordered_set.hpp>

using namespace drogon;

class ChatWebsocket final : public WebSocketController<ChatWebsocket>
{
public:
    ChatWebsocket()
    {
        // 使用 drogon::HttpAppFramework::instance()
        HttpAppFramework::instance().getLoop()->runEvery(10.0, [this] {
            sendHeartbeatToAll();
        });
        connToUser_.reserve(1000);
        userNameToConn_.reserve(1000);
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
    boost::unordered_map<std::string, WebSocketConnectionPtr> userNameToConn_;
    boost::unordered_map<WebSocketConnectionPtr, std::string> connToUser_;
    boost::unordered_set<std::string> excludedUsers_ = {"dog", "cat", "mouse"};
    mutable std::mutex mutex_;

    void sendHeartbeatToAll() const
    {
        //LOG_INFO << "sendHeartbeatToAll begin";
        std::lock_guard guard(mutex_);
        rd_kafka_topic_t* topic_ptr = kafka::KafkaManager::instance().getTopic("message_topic_one");
        chatRooms_.publish("001", std::format("房间公告消息"));

        for (const auto& [userName, wsConnPtr] : userNameToConn_)
        {
            if (!wsConnPtr->connected())
                continue;

            // 检查是否需要排除
            if (!excludedUsers_.contains(userName))
                continue;

            chatMessageVo messageVo{};
            messageVo.code = 200;
            messageVo.id = 0;
            messageVo.name = userName;
            messageVo.message = std::format("{} 心跳检测 正常 这是定制消息", userName);
            thread_local std::string buffer; // 使用 thread_local 避免频繁分配
            buffer.clear();
            (void)glz::write_json(messageVo, buffer);
            // 找到目标用户的连接，发送消息
            wsConnPtr->send(buffer);

            retryWithSleep([&]() {
                if (!kafka::KafkaManager::safeProduce(topic_ptr, buffer))
                {
                    const rd_kafka_resp_err_t err = rd_kafka_last_error();
                    LOG_ERROR << "Failed to produce message: " << rd_kafka_err2str(err);
                    if (err == RD_KAFKA_RESP_ERR__QUEUE_FULL)
                    {
                        return false;
                    }
                }
                return true;
            });
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