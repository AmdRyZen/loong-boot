#include "ChatWebsocket.h"
#include "utils/redisUtils.h"
#include "coroutinePool/TbbCoroutinePool.h"
//#include "user.pb.h"
#include <glaze/glaze.hpp>
#include <drogon/HttpAppFramework.h>
#include "utils/retry_utils.h"

struct Subscriber
{
    std::string topic_;
    SubscriberID id_{};
};

void ChatWebsocket::handleNewMessage(const WebSocketConnectionPtr& wsConn, std::string&& msg, const WebSocketMessageType& type)
{
    try
    {
        if (type == WebSocketMessageType::Ping)
        {
            wsConn->send("pong_ms", WebSocketMessageType::Pong);
            LOG_INFO << "Received a ping";
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

            if (!wsConn->disconnected())
            {
                const auto& subscriber = wsConn->getContextRef<Subscriber>();
                const auto& [topic, id] = subscriber;

                // 提交协程任务给协程池，协程自动启动，无需手动 resume
                // async_run([msg_dto, topic, id, this]() -> Task<>
                TbbCoroutinePool::instance().submit([msg_dto, topic, id, this]() -> AsyncTask
                {
                    try
                    {
                        std::string data{};
                        if (!msg_dto.key.empty())
                        {
                            // 异步调用 Redis 协程接口，示例用硬编码
                            // data = co_await redisUtils::getCoroRedisValue(std::format("get {}", msg_dto.key));
                            data = "xxxxxx";
                        }

                        if (!msg_dto.action.empty() && msg_dto.action == "message")
                        {
                            chatMessageVo msg_vo{};
                            msg_vo.code = 200;
                            msg_vo.id = id;
                            msg_vo.name = data;
                            msg_vo.message = msg_dto.msgContent;
                            thread_local std::string json;
                            json.clear();
                            (void)glz::write_json(msg_vo, json);

                            // 发布消息给订阅的客户端
                            chatRooms_.publish(topic, json);

                            // 异步发送 Kafka 消息，失败自动重试
                            co_await retryWithDelayAsync([]() -> Task<bool> {
                                if (rd_kafka_topic_t* topic_ptr = kafka::KafkaManager::instance().getTopic("message_topic"); !kafka::KafkaManager::safeProduce(topic_ptr, json))
                                {
                                    const rd_kafka_resp_err_t err = rd_kafka_last_error();
                                    LOG_ERROR << "Failed to produce message: " << rd_kafka_err2str(err);
                                    if (err == RD_KAFKA_RESP_ERR__QUEUE_FULL)
                                    {
                                        co_return false;
                                    }
                                }
                                co_return true;
                            },3, std::chrono::milliseconds(100));
                        }
                    }
                    catch (const std::exception& e)
                    {
                        LOG_ERROR << "Error in async task: " << e.what();
                    }
                    co_return;
                });
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
    Subscriber s;
    s.topic_ = req->getHeader("room_name");
    if (s.topic_.empty())
    {
        s.topic_ = "default_room";
    }
    std::string userName = req->getHeader("name");
    if (userName.empty())
    {
        userName = "default_name";
    }

    s.id_ = chatRooms_.subscribe(s.topic_, [weakWs = std::weak_ptr<WebSocketConnection>(wsConn)](const std::string&, const std::string& msg)
    {
        if (const auto web_socket_connection = weakWs.lock())
        {
            web_socket_connection->send(msg);
        }
    });

    std::lock_guard guard(mutex_);
    //connections_.emplace(wsConn);
    userNameConnections_.emplace(userName, wsConn.get());
    LOG_INFO << "Added connection for user: " << userName << "Subscriber ID: " << s.id_ << ", Topic: " << s.topic_;

    chatMessageVo msg_vo;
    msg_vo.code = 200;
    msg_vo.id = s.id_;
    msg_vo.name = s.topic_;
    msg_vo.message = std::format("欢迎 {} 加入我们 {}", userName, s.topic_);
    thread_local std::string json; // 使用 thread_local 避免频繁分配
    json.clear();
    (void)glz::write_json(msg_vo, json);
    chatRooms_.publish(s.topic_, json);

    rd_kafka_topic_t* topic_ptr = kafka::KafkaManager::instance().getTopic("message_topic");
    retryWithSleep([&]() {
        if (!kafka::KafkaManager::safeProduce(topic_ptr, json))
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

    wsConn->setContext(std::make_shared<Subscriber>(std::move(s)));
}

void ChatWebsocket::handleConnectionClosed(const WebSocketConnectionPtr& wsConn)
{
    try
    {
        std::string userName;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (const auto it = std::ranges::find_if(userNameConnections_, [&wsConn](const auto& pair) {
                    return pair.second == wsConn.get();
                });
                it != userNameConnections_.end())
            {
                userName = it->first;
                userNameConnections_.erase(it);
                LOG_INFO << "Removed user: " << userName;
            }
            if (userNameConnections_.empty())
            {
                // std::unordered_map 在元素删除后可能不会立即释放内存，导致内存占用较高。可以使用以下方法尝试释放未使用的内存
                userNameConnections_.clear();
                userNameConnections_.rehash(0);
            }
            //connections_.erase(wsConn);
            LOG_INFO << "Removed closed connection";
        }

        const auto& subscriber = wsConn->getContextRef<Subscriber>();
        const auto& [topic, id] = subscriber;
        chatRooms_.unsubscribe(topic, id);
        LOG_INFO << "Unsubscribed from topic: " << topic << ", ID: " << id;

        chatMessageVo msg_vo;
        msg_vo.code = 200;
        msg_vo.id = id;
        msg_vo.name = topic;
        msg_vo.message = std::format("{} 已离开 {}", userName, topic);
        thread_local std::string json; // 使用 thread_local 避免频繁分配
        json.clear();
        (void)glz::write_json(msg_vo, json);
        chatRooms_.publish(topic, json);

        rd_kafka_topic_t* topic_ptr = kafka::KafkaManager::instance().getTopic("message_topic");
        retryWithSleep([&]() {
            if (!kafka::KafkaManager::safeProduce(topic_ptr, json))
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
    catch (const std::exception& e)
    {
        LOG_ERROR << "Error in handleConnectionClosed: " << e.what();
    }
}