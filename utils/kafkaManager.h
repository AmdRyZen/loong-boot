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
    static KafkaManager &instance()
    {
        static KafkaManager instance;
        return instance;
    }

    void initialize(const std::string &brokers)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_)
        {
            conf_ = rd_kafka_conf_new();
            if (rd_kafka_conf_set(conf_, "bootstrap.servers", brokers.c_str(), errstr_, sizeof(errstr_)) != RD_KAFKA_CONF_OK)
            {
                throw std::runtime_error(std::string("Failed to configure Kafka broker: ") + errstr_);
            }

            // 设置自动提交偏移量为 true
            if (rd_kafka_conf_set(conf_, "enable.auto.commit", "true", errstr_, sizeof(errstr_)) != RD_KAFKA_CONF_OK) {
                throw std::runtime_error(std::string("Failed to set enable.auto.commit: ") + errstr_);
            }

            // 创建生产者
            producer_ = rd_kafka_new(RD_KAFKA_PRODUCER, conf_, errstr_, sizeof(errstr_));
            if (!producer_)
            {
                throw std::runtime_error(std::string("Failed to create Kafka producer: ") + errstr_);
            }

            // 创建消费者
            consumer_conf_ = rd_kafka_conf_new();
            if (rd_kafka_conf_set(consumer_conf_, "bootstrap.servers", brokers.c_str(), errstr_, sizeof(errstr_)) != RD_KAFKA_CONF_OK)
            {
                throw std::runtime_error(std::string("Failed to configure Kafka consumer: ") + errstr_);
            }

            if (rd_kafka_conf_set(consumer_conf_, "group.id", "my_consumer_group", errstr_, sizeof(errstr_)) != RD_KAFKA_CONF_OK)
            {
                throw std::runtime_error(std::string("Failed to configure Kafka group ID: ") + errstr_);
            }

            consumer_ = rd_kafka_new(RD_KAFKA_CONSUMER, consumer_conf_, errstr_, sizeof(errstr_));
            if (!consumer_)
            {
                throw std::runtime_error(std::string("Failed to create Kafka consumer: ") + errstr_);
            }

            initialized_ = true;
        }
    }

    rd_kafka_t *getProducer() const
    {
        if (!initialized_)
        {
            throw std::runtime_error("KafkaManager is not initialized");
        }
        return producer_;
    }

    rd_kafka_t *getConsumer() const
    {
        if (!initialized_)
        {
            throw std::runtime_error("KafkaManager is not initialized");
        }
        return consumer_;
    }

private:
    KafkaManager() = default;
    ~KafkaManager()
    {
        if (producer_)
        {
            rd_kafka_flush(producer_, 10 * 1000); // Wait for messages to be delivered
            rd_kafka_destroy(producer_);
        }
        if (consumer_)
        {
            rd_kafka_consumer_close(consumer_);
            rd_kafka_destroy(consumer_);
        }
    }

    rd_kafka_conf_t *conf_ = nullptr;
    rd_kafka_t *producer_ = nullptr;
    rd_kafka_conf_t *consumer_conf_ = nullptr;
    rd_kafka_t *consumer_ = nullptr;
    mutable std::mutex mutex_;
    char errstr_[512] = {};
    bool initialized_ = false;
};

#endif //KAFKAMANAGER_H
