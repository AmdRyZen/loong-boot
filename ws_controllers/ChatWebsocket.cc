#include "ChatWebsocket.h"
#include "utils/redisUtils.h"
#include "coroutinePool/TbbCoroutinePool.h"
#include "utils/PrometheusMetrics.h"
//#include "user.pb.h"
#include <glaze/glaze.hpp>
#include <drogon/HttpAppFramework.h>
#include "utils/retry_utils.h"
#include <memory_resource>
#include <thread>

struct Subscriber
{
    std::string topic_;
    std::string userName_;
    SubscriberID id_{};
};

void ChatWebsocket::produceKafkaAsync(std::string topicName, std::string payload)
{
    TbbCoroutinePool::instance().submit([topicName = std::move(topicName), payload = std::move(payload)] {
        rd_kafka_topic_t* topicPtr = kafka::KafkaManager::instance().getTopic(topicName);
        retryWithSleep([&]() {
            if (!kafka::KafkaManager::safeProduce(topicPtr, payload))
            {
                const rd_kafka_resp_err_t err = rd_kafka_last_error();
                LOG_ERROR << "Failed to produce message: " << rd_kafka_err2str(err);
                return err != RD_KAFKA_RESP_ERR__QUEUE_FULL;
            }
            return true;
        });
    });
}

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
                const std::string& topic = subscriber.topic_;
                const std::string& senderName = subscriber.userName_;
                const auto id = subscriber.id_;

                if (!msg_dto.action.empty() && msg_dto.action == "message")
                {
                    Metrics::PrometheusRegistry::instance().recordWsMessage();
                    // 异步提交给 TBB 纯净线程池：保持极速返回(38万+ QPS)，同时彻底杜绝 AsyncTask 协程帧泄漏
                    TbbCoroutinePool::instance().submit([this, topic, msg = std::move(msg_dto.msgContent), id, senderName]() {
                        chatMessageVo msg_vo{};
                        msg_vo.code = 200;
                        msg_vo.id = id;
                        msg_vo.name = std::string_view(senderName);
                        msg_vo.message = std::move(msg);

                        std::string json{};
                        (void)glz::write_json(msg_vo, json);

                        chatRooms_.publish(topic, json);
                    });
                }
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
        s.topic_ = req->getParameter("room_name");
        if (s.topic_.empty())
        {
            s.topic_ = "default_room";
        }
    }
    std::string userName = req->getHeader("name");
    if (userName.empty())
    {
        userName = req->getParameter("name");
        if (userName.empty())
        {
            userName = "default_name";
        }
    }
    s.userName_ = userName;

    s.id_ = chatRooms_.subscribe(s.topic_, [weakWs = std::weak_ptr<WebSocketConnection>(wsConn)](const std::string&, const std::string& msg)
    {
        if (const auto web_socket_connection = weakWs.lock())
        {
            web_socket_connection->send(msg);
        }
    });

    Metrics::PrometheusRegistry::instance().recordWsConnect();

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

    produceKafkaAsync("message_topic", json);

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
        const std::string& topic = subscriber.topic_;
        const auto id = subscriber.id_;
        chatRooms_.unsubscribe(topic, id);
        Metrics::PrometheusRegistry::instance().recordWsDisconnect();
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

        produceKafkaAsync("message_topic", json);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "Error in handleConnectionClosed: " << e.what();
    }
}
