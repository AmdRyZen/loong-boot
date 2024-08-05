#include "ChatWebsocket.h"
#include "utils/redisUtils.h"
#include "user.pb.h"
#include <glaze/glaze.hpp>
#include "../kafkaManager/kafkaManager.h"

struct Subscriber
{
    std::string chatRoomName_;
    SubscriberID id_{};
};

void ChatWebsocket::handleNewMessage(const WebSocketConnectionPtr& wsConnPtr, std::string&& message, const WebSocketMessageType& type)
{
    try
    {
        if (type == WebSocketMessageType::Ping)
        {
            wsConnPtr->send("pong", WebSocketMessageType::Pong);
            LOG_DEBUG << "recv a ping";
            return;
        }

        if (!message.empty())
        {
            chatMessageDto messageDto{};
            chatMessageVo errMessageVo{};

            if (glz::read_json(messageDto, message))
            {
                std::string buffer{};
                (void) glz::write_json(errMessageVo, buffer);
                wsConnPtr->send(buffer, WebSocketMessageType::Text);
                return;
            }

            // 在处理用户退出时检查连接状态
            if (!wsConnPtr->disconnected())
            {
                const auto& subscriber = wsConnPtr->getContextRef<Subscriber>();
                const auto& [chatRoomName, id] = subscriber;

                //auto sharedThis = shared_from_this();
                async_run([messageDto, chatRoomName, id, this]() -> Task<>
                {
                    try
                    {
                        std::string data{};
                        if (!messageDto.key.empty())
                        {
                            data = co_await redisUtils::getCoroRedisValue(std::format("get {}",  messageDto.key));
                        }

                        if (!messageDto.action.empty())
                        {
                            if (messageDto.action == "message")
                            {
                                // 发送消息到聊天室
                                std::string buffer{};
                                // protobuf
                                /*dto::UserData userData;
                                userData.set_id(std::to_string(id));
                                userData.set_name(action);
                                userData.set_message(msgContent);
                                userData.SerializeToString(&buffer);
                                // 清理 Protobuf 库
                                google::protobuf::ShutdownProtobufLibrary();*/

                                chatMessageVo messageVo{};
                                messageVo.code = 200;
                                messageVo.id = id;
                                messageVo.name = data;
                                messageVo.message = messageDto.msgContent;
                                // BEVE
                                (void) glz::write_json(messageVo, buffer);
                                chatRooms_.publish(chatRoomName, buffer);

                                {
                                    // 推送kfk 生产消息（异步）
                                    if (rd_kafka_produce(
                                           rd_kafka_topic_new(KafkaManager::instance().getProducer(), "message_topic", nullptr),
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
                            // 其他操作...
                        }
                    }
                    catch (const std::exception& e)
                    {
                        std::cerr << "Error in async task: " << e.what() << std::endl;
                    }
                    co_return;
                });
            }
        }
    }
    catch (...)
    {
        std::cout << "handleNewMessage ..." << std::endl;
    }
}
void ChatWebsocket::handleNewConnection(const HttpRequestPtr& req, const WebSocketConnectionPtr& wsConnPtr)
{
    //write your application logic here

    Subscriber s;
    s.chatRoomName_ = req->getHeader("room_name");
    if (s.chatRoomName_.empty()) {
        s.chatRoomName_ = "default_room"; // 设置默认的聊天室名称
    }
    std::string userName_ = req->getHeader("name");
    if (userName_.empty()) {
        userName_ = "default_name"; // 设置默认的名称
    }

    s.id_ = chatRooms_.subscribe(s.chatRoomName_,
                                 [wsConnPtr](const std::string& topic,
                                             const std::string& message) {
                                     // Supress unused variable warning
                                     (void)topic;
                                     wsConnPtr->send(message);
                                 });
    LOG_INFO << "id = " << s.id_;
    LOG_INFO << "chatRoomName = " << s.chatRoomName_;

    // 将新连接加入到连接列表
    {
        std::lock_guard<std::mutex> guard(mutex_);
        connections_.insert(wsConnPtr);
        // 用户ID
        userNames_[wsConnPtr] = userName_;
    }

    // 处理用户加入聊天室
    //wsConnPtr->send(std::format("欢迎 {} 加入我们 {}", userName_, s.chatRoomName_));
    chatRooms_.publish(s.chatRoomName_, std::format("欢迎 {} 加入我们 {}", userName_, s.chatRoomName_));

    chatMessageVo messageVo{};
    messageVo.code = 200;
    messageVo.id = s.id_;
    messageVo.name = s.chatRoomName_;
    messageVo.message = std::format("欢迎 {} 加入我们 {}", userName_, s.chatRoomName_);
    // BEVE
    std::string buffer{};
    (void) glz::write_json(messageVo, buffer);
    // 推送kfk 生产消息（异步）
    if (rd_kafka_produce(
           rd_kafka_topic_new(KafkaManager::instance().getProducer(), "message_topic", nullptr),
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

    wsConnPtr->setContext(std::make_shared<Subscriber>(std::move(s)));
}
void ChatWebsocket::handleConnectionClosed(const WebSocketConnectionPtr& wsConnPtr)
{
    //write your application logic here
    try
    {
        // 从连接列表中移除关闭的连接
        {
            std::lock_guard<std::mutex> guard(mutex_);
            connections_.erase(wsConnPtr);
            userNames_.erase(wsConnPtr);
        }
        // 获取Subscriber引用
        const auto& subscriber = wsConnPtr->getContextRef<Subscriber>();
        // 使用结构化绑定提取成员变量
        const auto& [chatRoomName, id] = subscriber;
        // 退出所有房间
        chatRooms_.unsubscribe(chatRoomName, id);
        // todo 暂时不确定是否需要
        /*if (chatRooms_.size() == 0)
        {
            std::cout << "chatRooms_.size() = " << chatRooms_.size() << std::endl;
            chatRooms_.clear();
        }*/
        // 清理资源
        //wsConnPtr->clearContext();
    }
    catch (...)
    {
        LOG_INFO << "handleConnectionClosed ...";
    }
}
