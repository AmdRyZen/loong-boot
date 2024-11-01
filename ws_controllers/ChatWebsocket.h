#pragma once
#include <drogon/PubSubService.h>
#include <drogon/WebSocketController.h>
#include "../kafkaManager/kafkaManager.h"
#include <glaze/glaze.hpp>

using namespace drogon;
class ChatWebsocket final : public WebSocketController<ChatWebsocket>
{
public:
    ChatWebsocket()
    {
        // 启动全局定时器
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
    // list path definitions here;
    // WS_PATH_ADD("/path","filter1","filter2",...);
    WS_PATH_ADD("/chat");
    WS_ADD_PATH_VIA_REGEX("/[^/]*", Get);
    WS_PATH_LIST_END

private:
    PubSubService<std::string> chatRooms_;
    std::unordered_set<WebSocketConnectionPtr> connections_;
    std::unordered_map<WebSocketConnectionPtr, std::string> userNames_; // 连接与用户名的映射
    mutable std::mutex mutex_;

    void sendHeartbeatToAll() const
    {
        // "001" 房间的name 可以从摸个集合或者Redis里面获取
        chatRooms_.publish("001", std::format("房间公告消息"));

        // 根据用户名发送定制消息
        std::lock_guard<std::mutex> guard(mutex_);
        const auto producer = rd_kafka_topic_new(KafkaManager::instance().getProducer(), "message_topic_one", nullptr);
        for (const auto &wsConnPtr : connections_)
        {
            if (wsConnPtr->connected())
            {
                std::string userName = userNames_.at(wsConnPtr); // 获取用户名
                chatMessageVo messageVo{};
                messageVo.code = 200;
                messageVo.id = 0;
                messageVo.name = userName;
                messageVo.message = std::format("{} 心跳检测 正常 这是定制消息", userName);
                std::string buffer{};
                (void) glz::write_json(messageVo, buffer);
                wsConnPtr->send(buffer);

                // 推送kfk 生产消息（异步）
                if (rd_kafka_produce(
                       producer,
                       RD_KAFKA_PARTITION_UA,
                       RD_KAFKA_MSG_F_COPY,
                       const_cast<char *>(buffer.data()), buffer.size(),
                       nullptr, 0,
                       nullptr) == -1)
                {
                    LOG_ERROR << "Failed to produce message: " << rd_kafka_err2str(rd_kafka_last_error());
                }
                // 处理确认和错误事件
                rd_kafka_poll(KafkaManager::instance().getProducer(), 0);
            }
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
