// ===== File: src/KafkaManager.cpp =====
#include "kafka/KafkaManager.h"
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <ranges>

using namespace kafka;

KafkaManager::KafkaManager() : producer_conf_(nullptr), producer_(nullptr), consumer_conf_(nullptr), initialized_(false), destroyed_(false), logCallback_(nullptr) {}
KafkaManager::~KafkaManager() { stop(); }
KafkaManager& KafkaManager::instance() {
    static KafkaManager mgr;
    return mgr;
}

 void KafkaManager::initialize(const std::string& brokers, const std::string& groupId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_) {
            std::cout << "KafkaManager already initialized with brokers: " << brokers << ", skipping." << std::endl;
            return;
        }

        char errStr[512];
        producer_conf_ = rd_kafka_conf_new();
        if (rd_kafka_conf_set(producer_conf_, "bootstrap.servers", brokers.c_str(), errStr, sizeof(errStr)) != RD_KAFKA_CONF_OK)
            throw std::runtime_error(std::string("Producer bootstrap.servers config error: ") + errStr);

        // 优化生产者配置
        rd_kafka_conf_set(producer_conf_, "queue.buffering.max.messages", "1000000", errStr, sizeof(errStr)); // 保持
        rd_kafka_conf_set(producer_conf_, "queue.buffering.max.kbytes", "2097152", errStr, sizeof(errStr)); // 保持
        rd_kafka_conf_set(producer_conf_, "batch.size", "1048576", errStr, sizeof(errStr)); // 保持
        rd_kafka_conf_set(producer_conf_, "linger.ms", "5", errStr, sizeof(errStr)); // 保持
        rd_kafka_conf_set(producer_conf_, "compression.type", "snappy", errStr, sizeof(errStr)); // 保持
        rd_kafka_conf_set(producer_conf_, "retries", "3", errStr, sizeof(errStr)); // 增加生产者重试机制
        rd_kafka_conf_set(producer_conf_, "delivery.timeout.ms", "30000", errStr, sizeof(errStr)); // 增加发送超时
        rd_kafka_conf_set(producer_conf_, "max.in.flight.requests.per.connection", "5", errStr, sizeof(errStr)); // 增加控制乱序问题

        rd_kafka_conf_set_dr_msg_cb(producer_conf_, KafkaManager::deliveryReportCallback);

        producer_ = rd_kafka_new(RD_KAFKA_PRODUCER, producer_conf_, errStr, sizeof(errStr));
        if (!producer_)
            throw std::runtime_error(std::string("Failed to create Kafka producer: ") + errStr);

        consumer_conf_ = rd_kafka_conf_new();
        if (rd_kafka_conf_set(consumer_conf_, "bootstrap.servers", brokers.c_str(), errStr, sizeof(errStr)) != RD_KAFKA_CONF_OK)
            throw std::runtime_error(std::string("Consumer bootstrap.servers config error: ") + errStr);
        if (rd_kafka_conf_set(consumer_conf_, "group.id", groupId.c_str(), errStr, sizeof(errStr)) != RD_KAFKA_CONF_OK)
            throw std::runtime_error(std::string("Consumer group.id config error: ") + errStr);
        if (rd_kafka_conf_set(consumer_conf_, "enable.auto.commit", "false", errStr, sizeof(errStr)) != RD_KAFKA_CONF_OK)
            throw std::runtime_error(std::string("Consumer enable.auto.commit config error: ") + errStr);

        rd_kafka_conf_set(consumer_conf_, "auto.offset.reset", "earliest", nullptr, 0);
        rd_kafka_conf_set(consumer_conf_, "session.timeout.ms", "60000", nullptr, 0);
        rd_kafka_conf_set(consumer_conf_, "heartbeat.interval.ms", "3000", nullptr, 0);

        rd_kafka_conf_set(consumer_conf_, "fetch.min.bytes", "1024", nullptr, 0);
        rd_kafka_conf_set(consumer_conf_, "fetch.max.wait.ms", "100", nullptr, 0);

        initialized_ = true;
        startPolling();
        std::cout << "KafkaManager initialized with brokers: " << brokers << ", group.id: " << groupId << std::endl;
    }

    bool KafkaManager::isHealthy() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return initialized_ && !destroyed_ && producer_;
    }

    rd_kafka_t* KafkaManager::getProducer() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || destroyed_)
            throw std::runtime_error("KafkaManager not initialized or destroyed");
        return producer_;
    }

    rd_kafka_t* KafkaManager::createNewConsumer() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || destroyed_)
            throw std::runtime_error("KafkaManager not initialized or destroyed");

        char errStr[512];
        rd_kafka_conf_t* confCopy = rd_kafka_conf_dup(consumer_conf_);
        rd_kafka_t* consumer = rd_kafka_new(RD_KAFKA_CONSUMER, confCopy, errStr, sizeof(errStr));
        if (!consumer) {
            rd_kafka_conf_destroy(confCopy);
            throw std::runtime_error(std::string("Failed to create Kafka consumer: ") + errStr);
        }
        std::cout << "Created new consumer instance." << std::endl;
        return consumer;
    }

    rd_kafka_topic_t* KafkaManager::getTopic(const std::string& topicName)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || destroyed_)
            throw std::runtime_error("KafkaManager not initialized or destroyed");
        if (topicName.empty()) {
            if (logCallback_) logCallback_("Error: Attempted to get empty topic name.");
            else std::cerr << "Error: Attempted to get empty topic name." << std::endl;
            throw std::runtime_error("Topic name cannot be empty");
        }

        //std::cout << "Accessing topic: " << topicName << ", exists: " << (topic_map_.contains(topicName) ? "yes" : "no") << std::endl;

        auto [it, inserted] = topic_map_.try_emplace(topicName, nullptr);
        if (inserted) {
            rd_kafka_topic_t* topic = rd_kafka_topic_new(producer_, topicName.c_str(), nullptr);
            if (!topic) {
                topic_map_.erase(topicName);
                throw std::runtime_error("Failed to create topic: " + topicName + ", error: " + rd_kafka_err2str(rd_kafka_last_error()));
            }
            it->second = topic;
            std::cout << "Created new topic: " << topicName << std::endl;
        }
        return it->second;
    }

    bool KafkaManager::safeProduce(rd_kafka_topic_t* topic, const std::string& message)
    {
        auto* payload = strdup(message.c_str());
        if (!payload)
        {
            return false;
        }

        if (rd_kafka_produce(
                topic,
                RD_KAFKA_PARTITION_UA,
                RD_KAFKA_MSG_F_FREE,
                payload, message.size(),
                nullptr, 0,
                nullptr) == -1)
        {
            //rd_kafka_resp_err_t err = rd_kafka_last_error();
            free(payload);  // 释放内存避免泄漏
            return false;
        }

        return true;
    }

    void KafkaManager::stop()
    {
        stopPolling();
        std::lock_guard<std::mutex> lock(mutex_);
        if (destroyed_) {
            std::cout << "KafkaManager already stopped." << std::endl;
            return;
        }
        destroyed_ = true;

        std::cout << "Stopping KafkaManager, cleaning up " << topic_map_.size() << " topics." << std::endl;
        std::ranges::for_each(topic_map_ | std::views::values, [](auto* topic) {
            if (topic)
                rd_kafka_topic_destroy(topic);
        });
        topic_map_.clear();

        if (producer_) {
            rd_kafka_flush(producer_, 5000); // 增加 flush 超时
            rd_kafka_destroy(producer_);
            producer_ = nullptr;
        }
        if (consumer_conf_) {
            rd_kafka_conf_destroy(consumer_conf_);
            consumer_conf_ = nullptr;
        }
        if (producer_conf_) {
            rd_kafka_conf_destroy(producer_conf_);
            producer_conf_ = nullptr;
        }
    }

    void KafkaManager::startPolling()
    {
        running_ = true;
        pollThread_ = std::thread([this]() {
            while (running_) {
                try {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (producer_ && !destroyed_) {
                        rd_kafka_poll(producer_, 50); // 缩短轮询间隔
                    }
                } catch (const std::exception& e) {
                    if (logCallback_) logCallback_(std::string("Exception in poll thread: ") + e.what());
                    else std::cerr << "Exception in poll thread: " << e.what() << std::endl;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 缩短休眠时间
            }
        });
        std::cout << "Started Kafka polling thread." << std::endl;
    }

    void KafkaManager::stopPolling()
    {
        running_ = false;
        if (pollThread_.joinable()) {
            pollThread_.join();
            std::cout << "Stopped Kafka polling thread." << std::endl;
        }
    }

    void KafkaManager::deliveryReportCallback(rd_kafka_t* rk, const rd_kafka_message_t* rkmessage, void* opaque)
    {
        if (rkmessage->err) {
            if (instance().logCallback_) instance().logCallback_(std::string("Message delivery failed: ") + rd_kafka_err2str(rkmessage->err));
            else std::cerr << "Message delivery failed: " << rd_kafka_err2str(rkmessage->err) << std::endl;
        } else {
            /*std::cout << "Message delivered to topic " << rd_kafka_topic_name(rkmessage->rkt)
                      << " [" << rkmessage->partition << "] at offset " << rkmessage->offset << std::endl;*/
        }
    }
