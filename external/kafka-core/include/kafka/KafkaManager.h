// ===== File: include/kafka/KafkaManager.h =====
#ifndef KAFKA_CORE_KAFKA_MANAGER_H
#define KAFKA_CORE_KAFKA_MANAGER_H

#pragma once

#include <rdkafka.h>
#include <string>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <functional>

namespace kafka {

class KafkaManager {
public:
    static KafkaManager& instance();

    void initialize(const std::string& brokers, const std::string& groupId = "default_group");
    bool isHealthy() const;
    rd_kafka_t* getProducer() const;
    rd_kafka_t* createNewConsumer() const;
    rd_kafka_topic_t* getTopic(const std::string& topicName);
    static bool safeProduce(rd_kafka_topic_t* topic, const std::string& message);
    void stop();

    KafkaManager(const KafkaManager&) = delete;
    KafkaManager& operator=(const KafkaManager&) = delete;

private:
    KafkaManager();
    ~KafkaManager();
    void startPolling();
    void stopPolling();
    static void deliveryReportCallback(rd_kafka_t* rk, const rd_kafka_message_t* rkmessage, void* opaque);

private:
    mutable std::mutex mutex_;
    rd_kafka_conf_t* producer_conf_;
    rd_kafka_t* producer_;
    rd_kafka_conf_t* consumer_conf_;
    bool initialized_;
    std::atomic_bool destroyed_;
    std::unordered_map<std::string, rd_kafka_topic_t*> topic_map_;
    std::atomic_bool running_{false};
    std::thread pollThread_;
    std::function<void(const std::string&)> logCallback_;
};

} // namespace kafka

#endif // KAFKA_CORE_KAFKA_MANAGER_H