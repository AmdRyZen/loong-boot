//
// Created by 神圣•凯莎 on 24-7-30.
//

#ifndef KAFKAMANAGER_H
#define KAFKAMANAGER_H

#pragma once

#include <rdkafka.h>
#include <string>
#include <mutex>

class KafkaManager
{
public:
    // 获取单例实例
    static KafkaManager& instance()
    {
        static KafkaManager mgr;
        return mgr;
    }

    // 初始化配置（必须先调用一次）
    void initialize(const std::string& brokers, const std::string& groupId = "default_group")
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_)
            return;

        char errStr[512];

        // 生产者配置
        producer_conf_ = rd_kafka_conf_new();

        if (rd_kafka_conf_set(producer_conf_, "bootstrap.servers", brokers.c_str(), errStr, sizeof(errStr)) != RD_KAFKA_CONF_OK)
            throw std::runtime_error(std::string("Producer bootstrap.servers config error: ") + errStr);

        // 这里可以根据需要添加生产者更多配置...

        producer_ = rd_kafka_new(RD_KAFKA_PRODUCER, producer_conf_, errStr, sizeof(errStr));
        if (!producer_)
            throw std::runtime_error(std::string("Failed to create Kafka producer: ") + errStr);

        // 消费者配置
        consumer_conf_ = rd_kafka_conf_new();

        // 设置 brokers
        if (rd_kafka_conf_set(consumer_conf_, "bootstrap.servers", brokers.c_str(), errStr, sizeof(errStr)) != RD_KAFKA_CONF_OK)
            throw std::runtime_error(std::string("Consumer bootstrap.servers config error: ") + errStr);

        // 设置消费者组ID
        if (rd_kafka_conf_set(consumer_conf_, "group.id", groupId.c_str(), errStr, sizeof(errStr)) != RD_KAFKA_CONF_OK)
            throw std::runtime_error(std::string("Consumer group.id config error: ") + errStr);

        // 关闭自动提交，改为手动提交
        if (rd_kafka_conf_set(consumer_conf_, "enable.auto.commit", "false", errStr, sizeof(errStr)) != RD_KAFKA_CONF_OK)
            throw std::runtime_error(std::string("Consumer enable.auto.commit config error: ") + errStr);

        // 消费者超时等常用配置
        rd_kafka_conf_set(consumer_conf_, "auto.offset.reset", "earliest", nullptr, 0);
        rd_kafka_conf_set(consumer_conf_, "session.timeout.ms", "60000", nullptr, 0);
        rd_kafka_conf_set(consumer_conf_, "heartbeat.interval.ms", "3000", nullptr, 0);

        // 注意：不设置 socket_cb，采用轮询模式

        initialized_ = true;

        std::cout << "KafkaManager initialized with brokers: " << brokers << std::endl;
    }

    // 获取单例生产者实例
    rd_kafka_t* getProducer() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_)
            throw std::runtime_error("KafkaManager not initialized");
        return producer_;
    }

    // 创建一个新的消费者实例（线程安全）
    rd_kafka_t* createNewConsumer() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_)
            throw std::runtime_error("KafkaManager not initialized");

        char errStr[512];
        rd_kafka_conf_t* confCopy = rd_kafka_conf_dup(consumer_conf_);

        rd_kafka_t* consumer = rd_kafka_new(RD_KAFKA_CONSUMER, confCopy, errStr, sizeof(errStr));
        if (!consumer)
        {
            rd_kafka_conf_destroy(confCopy); // 确保清理副本
            throw std::runtime_error(std::string("Failed to create Kafka consumer: ") + errStr);
        }

        return consumer;
    }

    // 禁止拷贝和赋值
    KafkaManager(const KafkaManager&) = delete;
    KafkaManager& operator=(const KafkaManager&) = delete;

private:
    KafkaManager() : producer_conf_(nullptr), producer_(nullptr), consumer_conf_(nullptr), initialized_(false) {}
    ~KafkaManager()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (producer_)
        {
            rd_kafka_flush(producer_, 1000);
            rd_kafka_destroy(producer_);
        }
        if (consumer_conf_)
        {
            rd_kafka_conf_destroy(consumer_conf_);
            consumer_conf_ = nullptr;
        }
        if (producer_conf_)
        {
            rd_kafka_conf_destroy(producer_conf_);
            producer_conf_ = nullptr;
        }
        // 注意每个创建的 consumer 由调用者管理销毁
    }

    mutable std::mutex mutex_;

    rd_kafka_conf_t* producer_conf_;
    rd_kafka_t* producer_;

    rd_kafka_conf_t* consumer_conf_;

    bool initialized_;
};

#endif //KAFKAMANAGER_H
