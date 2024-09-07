//
// Created by 神圣•凯莎 on 24-9-6.
//

#ifndef MQTTCONSUMER_H
#define MQTTCONSUMER_H

#pragma once

#include <mqtt/async_client.h>
#include <thread>
#include <atomic>
#include <vector>

class MqttConsumer {
public:
    explicit MqttConsumer(const size_t numThreads = 1)
        : client_(MqttManager::instance().getClient()), stop_(false) {
        try {
            // 订阅主题
            client_->subscribe("topic", 1)->wait();

            // 启动多个线程来消费消息
            for (size_t i = 0; i < numThreads; ++i) {
                consumerThreads_.emplace_back(&MqttConsumer::consumeMessages, this);
            }
        } catch (const mqtt::exception& exc) {
            LOG_ERROR << "Error subscribing to topic: " << exc.what();
        }
    }

    void stop() {
        stop_ = true;
        for (auto& thread : consumerThreads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    ~MqttConsumer() {
        stop();
    }

  private:
    void consumeMessages()const {
        client_->start_consuming();
        while (!stop_) {
            if (const auto msg = client_->consume_message()) {
                handleMqttMessage(msg->to_string());
            }
        }
        client_->stop_consuming();
    }

    static void handleMqttMessage(const std::string& message) {
        LOG_INFO << "Message received: " << message;
    }

    mqtt::async_client* client_;
    std::vector<std::thread> consumerThreads_;
    std::atomic<bool> stop_;
};

#endif // MQTTCONSUMER_H