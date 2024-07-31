//
// Created by 神圣•凯莎 on 24-7-30.
//

#pragma once

#include <rdkafka.h>
#include <iostream>
#include <thread>
#include <atomic>

using namespace drogon;

class AsyncKafkaConsumer
{
public:
    explicit AsyncKafkaConsumer(const size_t numThreads = 4) : stop_(false)
    {
        try
        {
            // 启动多个后台线程来异步消费消息
            for (size_t i = 0; i < numThreads; ++i)
            {
                // 创建并启动 Kafka 消费线程
                consumerThreads_.emplace_back(&AsyncKafkaConsumer::consumeMessages, this);
            }
            std::cout << "Kafka consumer started." << std::endl;
        }
        catch (const std::exception &e)
        {
            std::cerr << "Exception in AsyncKafkaConsumer constructor: " << e.what() << std::endl;
            // 可能需要进一步处理异常，例如重新尝试初始化
        }
    }

    ~AsyncKafkaConsumer()
    {
        stop_ = true;
        for (auto& thread : consumerThreads_)
        {
            if (thread.joinable())
            {
                thread.join(); // 等待线程完成
            }
        }
        std::cout << "Kafka consumer stopped." << std::endl;
    }

private:
    void consumeMessages() const
    {
        const auto consumer = KafkaManager::instance().getConsumer();
        // 创建一个新的 topic partition list
        rd_kafka_topic_partition_list_t *partitions = rd_kafka_topic_partition_list_new(1);
        // 向列表中添加主题和分区
        rd_kafka_topic_partition_list_add(partitions, "message_topic", RD_KAFKA_PARTITION_UA);
        // 订阅主题
        const rd_kafka_resp_err_t err = rd_kafka_subscribe(consumer, partitions);
        // 销毁 topic partition list
        rd_kafka_topic_partition_list_destroy(partitions);

        if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            std::cerr << "Failed to subscribe to topic: " << rd_kafka_err2str(err) << std::endl;
            return;
        }

        while (!stop_)
        {
            if (rd_kafka_message_t *msg = rd_kafka_consumer_poll(consumer, 1000)) // Poll every second)
            {
                if (msg->err)
                {
                    if (msg->err == RD_KAFKA_RESP_ERR__PARTITION_EOF)
                    {
                        // 当前分区的消息已经消费完毕
                        std::cout << "Reached end of partition." << std::endl;
                    }
                    else
                    {
                        std::cerr << "Error consuming message: " << rd_kafka_err2str(msg->err) << std::endl;
                    }
                }
                else
                {
                    std::string message(static_cast<const char*>(msg->payload), msg->len);

                    // 启动协程处理消息
                    async_run([message, msg, consumer, this]() -> Task<> {
                        try
                        {
                            LOG_INFO << "收到消息 Received message: " << message;

                            // 处理完消息后手动提交偏移量
                            rd_kafka_commit_message(consumer, msg, 0);
                        }
                        catch (const std::exception& ex)
                        {
                            LOG_ERROR << "Exception while processing message: " << ex.what();
                        }
                        co_return;
                    });
                }
                rd_kafka_message_destroy(msg); // 释放消息资源
            }
        }
        rd_kafka_consumer_close(consumer);
    }

    std::vector<std::thread> consumerThreads_; // Kafka 消费线程
    std::atomic<bool> stop_{false}; // 控制消费线程的停止
};