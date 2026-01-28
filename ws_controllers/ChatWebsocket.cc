#include "ChatWebsocket.h"
#include "utils/redisUtils.h"
#include "coroutinePool/TbbCoroutinePool.h"
//#include "user.pb.h"
#include <glaze/glaze.hpp>
#include <drogon/HttpAppFramework.h>
#include "utils/retry_utils.h"
#include <memory_resource>
#include <thread>

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

                // 不在协程中使用thread_local的monotonic_buffer_resource
                // 改为使用标准字符串
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
                            // 使用标准字符串，避免pmr相关问题
                            chatMessageVo msg_vo{};
                            msg_vo.code = 200;
                            msg_vo.id = id;
                            msg_vo.name = data;  // 使用普通string
                            msg_vo.message = msg_dto.msgContent;  // 使用普通string

                            std::string json{};  // 使用普通string
                            (void)glz::write_json(msg_vo, json);

                            // 发布消息给订阅的客户端
                            chatRooms_.publish(topic, json);

                            // 异步发送 Kafka 消息，失败自动重试
                            /*co_await retryWithDelayAsync([json]() -> Task<bool> {
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
                            },3, std::chrono::milliseconds(100));*/
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

    // 使用原子操作或无锁数据结构来减少锁竞争
    {
        std::lock_guard guard(mutex_);
        userNameToConn_.emplace(userName, wsConn);
        connToUser_.emplace(wsConn, userName);
    }

    LOG_INFO << "Added connection for user: " << userName << " Subscriber ID: " << s.id_ << ", Topic: " << s.topic_;

    chatMessageVo msg_vo;
    msg_vo.code = 200;
    msg_vo.id = s.id_;
    msg_vo.name = s.topic_;
    msg_vo.message = std::format("欢迎 {} 加入我们 {}", userName, s.topic_);

    // 使用普通string避免thread_local问题
    std::string json{};
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
            if (const auto it = connToUser_.find(wsConn); it != connToUser_.end())
            {
                userName = it->second;
                connToUser_.erase(it);
                userNameToConn_.erase(userName);
                LOG_INFO << "Removed user: " << userName;
            }

            // 仅在容器很大时才尝试收缩内存
            if (userNameToConn_.size() > 1000) {
                userNameToConn_.rehash(0); // 尝试释放多余内存
            }
            if (connToUser_.size() > 1000) {
                connToUser_.rehash(0);
            }

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

        // 使用普通string避免thread_local问题
        std::string json{};
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