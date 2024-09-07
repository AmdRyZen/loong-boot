//
// Created by 神圣•凯莎 on 24-9-6.
//

#ifndef MQTTMANAGER_H
#define MQTTMANAGER_H

#pragma once

#include <mqtt/async_client.h>
#include <memory>

class MqttManager {
public:
    static MqttManager& instance() {
        static MqttManager instance;
        return instance;
    }

    void initialize(const std::string& serverAddress, const std::string& clientId) {
        client_ = std::make_unique<mqtt::async_client>(serverAddress, clientId);
        mqtt::connect_options connOpts;
        connOpts.set_clean_session(true);

        try {
            client_->connect(connOpts)->wait();
            LOG_INFO << "Connected to the MQTT broker.";
        } catch (const mqtt::exception& exc) {
            LOG_ERROR << "Error: " << exc.what();
        }
    }

    void publish(const std::string& topic, const std::string& message) const {
        const auto msg = mqtt::make_message(topic, message);
        msg->set_qos(1);
        client_->publish(msg)->wait();
    }

    MqttManager()
    {
        // 启动全局定时器
        HttpAppFramework::instance().getLoop()->runEvery(10.0, [this] {
            //instance().publish("topic", "MQTT 心跳检测!");
        });
    }

    ~MqttManager() {
        try {
            LOG_INFO << "Disconnecting from MQTT broker...";
            if (client_) {
                client_->disconnect()->wait();
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
    std::unique_ptr<mqtt::async_client> client_;
};

#endif // MQTTMANAGER_H