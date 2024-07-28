#include "ChatWebsocket.h"
#include "utils/redisUtils.h"
#include "user.pb.h"
#include <glaze/glaze.hpp>

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
    std::cout << "handleNewConnection" << std::endl;

    Subscriber s;
    s.chatRoomName_ = req->getHeader("room_name");
    if (s.chatRoomName_.empty()) {
        s.chatRoomName_ = "default_room"; // 设置默认的聊天室名称
    }
    std::string userName_ = req->getHeader("name");
    if (userName_.empty()) {
        userName_ = "default_name"; // 设置默认的名称
    }
    // 处理用户加入聊天室
    //wsConnPtr->send(std::format("欢迎 {} 加入我们 {}", userName_, s.chatRoomName_));
    chatRooms_.publish(s.chatRoomName_, std::format("欢迎 {} 加入我们 {}", userName_, s.chatRoomName_));

    // 主动向客户端发送定时消息 并保存定时器ID
    const auto timerId = HttpAppFramework::instance().getLoop()->runEvery(10.0, [s, userName_, wsConnPtr, this] {
        //chatRooms_.publish(s.chatRoomName_, std::format("心跳检测 {} 正常", s.chatRoomName_));
        wsConnPtr->send(std::format("{} 心跳检测 {} 正常", userName_, s.chatRoomName_));
    });
    // 将定时器ID与连接关联
    timers_[wsConnPtr] = timerId;

    s.id_ = chatRooms_.subscribe(s.chatRoomName_,
                                 [wsConnPtr](const std::string& topic,
                                             const std::string& message) {
                                     // Supress unused variable warning
                                     (void)topic;
                                     wsConnPtr->send(message);
                                 });
    std::cout << "id = " << s.id_ << std::endl;
    std::cout << "chatRoomName = " << s.chatRoomName_ << std::endl;
    wsConnPtr->setContext(std::make_shared<Subscriber>(std::move(s)));
}
void ChatWebsocket::handleConnectionClosed(const WebSocketConnectionPtr& wsConnPtr)
{
    //write your application logic here
    try
    {
        // 取消定时任务
        const auto it = timers_.find(wsConnPtr);
        if (it != timers_.end())
        {
            HttpAppFramework::instance().getLoop()->invalidateTimer(it->second);
            timers_.erase(it);
        }

        //std::cout << "handleConnectionClosed" << std::endl;
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
        wsConnPtr->clearContext();

        std::cout << "handleConnectionClosed id = " << id << std::endl;
        std::cout << "handleConnectionClosed chatRoomName = " << chatRoomName << std::endl;
    }
    catch (...)
    {
        std::cout << "handleConnectionClosed ..." << std::endl;
    }
}
