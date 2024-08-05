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
            // 创建生产者
            producer_conf_.reset(rd_kafka_conf_new());

            // 	request.timeout.ms: 请求超时。客户端在等待响应时的最大时间。设置为 30s 或更长时间。
            rd_kafka_conf_set(producer_conf_.get(), "request.timeout.ms", "30000", nullptr, 0);

            // offset.commit.interval.ms: 手动提交偏移量的时间间隔，通常设置为 60s。
            rd_kafka_conf_set(producer_conf_.get(), "offset.commit.interval.ms", "60000", nullptr, 0);

            // acks: 确定生产者写入操作的确认级别。all 表示所有副本都确认写入后才算成功。
            rd_kafka_conf_set(producer_conf_.get(), "acks", "all", nullptr, 0);

            // linger.ms: 生产者在发送消息前等待的时间，以便聚集更多的消息。默认值通常为 0，生产环境中可以根据需求调整，建议设置为 5ms 到 100ms 之间。
            rd_kafka_conf_set(producer_conf_.get(), "linger.ms", "10", nullptr, 0);

            // batch.size: 批量大小。生产者在发送消息时将消息聚集在一起的大小。通常设置为 16KB 到 64KB。
            rd_kafka_conf_set(producer_conf_.get(), "batch.size", "16384", nullptr, 0);

            // log.level: Kafka 客户端的日志级别。info 或 debug 级别可以帮助调试问题，但生产环境中可能需要设置为 warn 或 error 以减少日志量。
            rd_kafka_conf_set(producer_conf_.get(), "log.level", "info", nullptr, 0);

            // log.connection.close: 是否记录连接关闭日志。
            rd_kafka_conf_set(producer_conf_.get(), "log.connection.close", "false", nullptr, 0);

            // 	bootstrap.servers: 指定 Kafka 集群的地址。
            if (rd_kafka_conf_set(producer_conf_.get(), "bootstrap.servers", brokers.c_str(), errstr_, sizeof(errstr_)) != RD_KAFKA_CONF_OK)
            {
                throw std::runtime_error(std::string("Failed to configure Kafka broker: ") + errstr_);
            }

            producer_.reset(rd_kafka_new(RD_KAFKA_PRODUCER, producer_conf_.get(), errstr_, sizeof(errstr_)));
            if (!producer_)
            {
                throw std::runtime_error(std::string("Failed to create Kafka producer: ") + errstr_);
            }

            // 创建消费者
            consumer_conf_.reset(rd_kafka_conf_new());

            // enable.auto.commit: 自动提交偏移量的开关。生产环境中通常建议手动提交，以更好地控制偏移量的提交。
            rd_kafka_conf_set(consumer_conf_.get(), "enable.auto.commit", "false", nullptr, 0);

            // auto.commit.interval.ms: 自动提交偏移量的时间间隔。如果 enable.auto.commit 设置为 true，则此配置项生效。
            rd_kafka_conf_set(consumer_conf_.get(), "auto.commit.interval.ms", "5000", nullptr, 0);

            // 	session.timeout.ms: 消费者组会话超时。消费者在多长时间没有发送心跳时，协调器会将其从组中移除。生产环境中通常设置为 60s 或更长。
            rd_kafka_conf_set(consumer_conf_.get(), "session.timeout.ms", "60000", nullptr, 0);

            // heartbeat.interval.ms: 心跳间隔。消费者发送心跳的频率。建议设置为 session.timeout.ms 的一半或更少，例如 3s。
            rd_kafka_conf_set(consumer_conf_.get(), "heartbeat.interval.ms", "3000", nullptr, 0);

            // auto.offset.reset: 指定当没有初始偏移量时应该如何开始消费。earliest 表示从最早的消息开始。
            rd_kafka_conf_set(consumer_conf_.get(), "auto.offset.reset", "earliest", nullptr, 0);

            // max.poll.records：控制每次拉取的记录数量。增加此值可以减少拉取请求的次数，但可能增加每次处理的负担：
            rd_kafka_conf_set(consumer_conf_.get(), "max.poll.records", "1000", nullptr, 0);

            // fetch.min.bytes 和 fetch.max.wait.ms：这些配置影响消息的拉取频率和批量大小。可以尝试调整这些配置来优化性能：
            rd_kafka_conf_set(consumer_conf_.get(), "fetch.min.bytes", "4096", nullptr, 0);
            rd_kafka_conf_set(consumer_conf_.get(), "fetch.max.wait.ms", "200", nullptr, 0);

            if (rd_kafka_conf_set(consumer_conf_.get(), "bootstrap.servers", brokers.c_str(), errstr_, sizeof(errstr_)) != RD_KAFKA_CONF_OK)
            {
                throw std::runtime_error(std::string("Failed to configure Kafka consumer: ") + errstr_);
            }

            if (rd_kafka_conf_set(consumer_conf_.get(), "group.id", "my_consumer_group", errstr_, sizeof(errstr_)) != RD_KAFKA_CONF_OK)
            {
                throw std::runtime_error(std::string("Failed to configure Kafka group ID: ") + errstr_);
            }

            consumer_.reset(rd_kafka_new(RD_KAFKA_CONSUMER, consumer_conf_.get(), errstr_, sizeof(errstr_)));
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
        return producer_.get();
    }

    rd_kafka_t *getConsumer() const
    {
        if (!initialized_)
        {
            throw std::runtime_error("KafkaManager is not initialized");
        }
        return consumer_.get();
    }

private:
    KafkaManager() = default;
    ~KafkaManager()
    {
        if (producer_)
        {
            rd_kafka_flush(producer_.get(),  1000); // 等待消息传输完成
            rd_kafka_destroy(producer_.get());
            producer_.reset();
        }
        if (consumer_)
        {
            rd_kafka_consumer_close(consumer_.get());
            rd_kafka_flush(consumer_.get(), 1000); // 等待消息传输完成
            rd_kafka_destroy(consumer_.get());
            consumer_.reset();
        }
    }

    mutable std::mutex mutex_;
    std::unique_ptr<rd_kafka_conf_t, decltype(&rd_kafka_conf_destroy)> producer_conf_{nullptr, &rd_kafka_conf_destroy};
    std::unique_ptr<rd_kafka_t, decltype(&rd_kafka_destroy)> producer_{nullptr, &rd_kafka_destroy};
    std::unique_ptr<rd_kafka_conf_t, decltype(&rd_kafka_conf_destroy)> consumer_conf_{nullptr, &rd_kafka_conf_destroy};
    std::unique_ptr<rd_kafka_t, decltype(&rd_kafka_destroy)> consumer_{nullptr, &rd_kafka_destroy};
    char errstr_[512] = {};
    bool initialized_ = false;
};

#endif //KAFKAMANAGER_H
