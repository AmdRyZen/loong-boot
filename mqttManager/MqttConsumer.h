//
// Created by 神圣•凯莎 on 24-9-6.
//

#ifndef MQTTCONSUMER_H
#define MQTTCONSUMER_H

#pragma once

#include <mqtt/async_client.h>
#include <atomic>
#include <stdexcept>
#include "MqttManager.h" // 确保包含 MqttManager 的头文件

extern ThreadPool poolMqtt;
using namespace drogon;
class MqttConsumer
{
  public:
    explicit MqttConsumer(const size_t numThreads = 2)
      : stop_(false)
    {
        try
        {
            // 获取 MQTT 客户端
            client_ = MqttManager::instance().getClient();
            if (!client_)
            {
                throw std::runtime_error("MQTT client is not initialized.");
            }
            // 订阅主题
            client_->start_consuming();
            client_->subscribe("topic", 1)->wait();

            // 启动线程池处理消息
            poolMqtt.enqueue(&MqttConsumer::consumeMessages, this);
        }
        catch (const mqtt::exception& exc)
        {
            LOG_ERROR << "Error subscribing to topic: " << exc.what();
        }
        catch (const std::exception& ex)
        {
            LOG_ERROR << "Exception in MqttConsumer constructor: " << ex.what();
        }
    }

    void stop() {
        stop_ = true;
        client_->stop_consuming(); // 确保停止消费消息
        // 等待线程池中的所有任务完成
    }

    ~MqttConsumer()
    {
        stop();
    }

private:
    void consumeMessages()const {
        while (!stop_) {
            try {
                if (const auto msg = client_->consume_message();msg) {
                    // 启动协程处理消息
                    async_run([this, msg]() -> Task<> {
                        try
                        {
                            handleMqttMessage(msg->to_string());
                        }
                        catch (const std::exception& ex)
                        {
                            LOG_ERROR << "Exception async_run processing message: " << ex.what();
                        }
                        co_return;
                    });
                }
            } catch (const mqtt::exception& exc) {
                LOG_ERROR << "Error while consuming message: " << exc.what();
            }
        }
    }

    static void handleMqttMessage(const std::string& message) {
        LOG_INFO << "Message received: " << message;
    }

    mqtt::async_client* client_{nullptr};
    std::atomic<bool> stop_;
};

#endif // MQTTCONSUMER_H