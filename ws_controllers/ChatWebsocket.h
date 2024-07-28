#pragma once
#include <drogon/PubSubService.h>
#include <drogon/WebSocketController.h>

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
    WS_PATH_LIST_END

private:
    PubSubService<std::string> chatRooms_;
    std::unordered_map<WebSocketConnectionPtr, trantor::TimerId> timers_;


    void sendHeartbeatToAll() const
    {
        // "001" 房间的name 可以从摸个集合或者Redis里面获取
        chatRooms_.publish("001", std::format("房间公告消息"));
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
