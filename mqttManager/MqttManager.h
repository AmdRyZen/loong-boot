//
// Created by 神圣•凯莎 on 24-9-6.
//

#ifndef MQTTMANAGER_H
#define MQTTMANAGER_H

#pragma once

#include <mqtt/async_client.h>
#include <string>

class MqttManager {
public:
    static MqttManager& instance() {
        static MqttManager instance;
        return instance;
    }

    void initialize(const std::string& serverAddress, const std::string& clientId) {
        client_ = std::make_unique<mqtt::async_client>(serverAddress, clientId);
        mqtt::connect_options connOpts;
        connOpts.set_keep_alive_interval(20);  // 设置保活间隔为 20 秒
        connOpts.set_clean_session(true);    // 清理会话

        try {
            client_->connect(connOpts)->wait();
            LOG_INFO << "Connected to the MQTT broker.";
        } catch (const mqtt::exception& exc) {
            LOG_ERROR << "Error: " << exc.what();
        }
    }

    void publish(const std::string& topic, const std::string& message) const {
        if (client_) {
            const auto msg = mqtt::make_message(topic, message);
            msg->set_qos(1);
            msg->set_retained(false); // false：消息不被服务器保留，只有当前订阅者会收到。
            client_->publish(msg)->wait();
        } else {
            LOG_ERROR << "MQTT client is not initialized.";
        }
    }

    MqttManager() : client_(nullptr) {
        // 启动全局定时器
        HttpAppFramework::instance().getLoop()->runEvery(10.0, [this] {
            Example: instance().publish("topic", "MQTT 心跳检测!");
        });
    }

    ~MqttManager() {
        try {
            LOG_INFO << "Disconnecting from MQTT broker...";
            if (client_) {
                if (client_->is_connected()) {
                    client_->disconnect()->wait();
                }
                client_.reset();
            }
            LOG_INFO << "Disconnected from the MQTT broker.";
        } catch (const mqtt::exception& e) {
            LOG_ERROR << "Exception during destruction: " << e.what();
        }
    }

    [[nodiscard]] mqtt::async_client* getClient() const {
        return client_.get();
    }

private:
    std::unique_ptr<mqtt::async_client> client_; // 使用智能指针管理生命周期
};

#endif // MQTTMANAGER_H