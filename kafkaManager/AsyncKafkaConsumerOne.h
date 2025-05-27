#pragma once

#include <rdkafka.h>
#include <atomic>

extern ThreadPool poolKafkaOne;
using namespace drogon;
class AsyncKafkaConsumerOne
{
public:
    explicit AsyncKafkaConsumerOne(const size_t numThreads = 4) : stop_(false)
    {
        try
        {
            // 启动多个后台线程来异步消费消息
            for (size_t i = 0; i < numThreads; ++i)
            {
                // 为每个线程创建独立的 Kafka 消费者实例
                rd_kafka_t* consumer = KafkaManager::instance().createNewConsumer();
                consumers_.push_back(consumer);
                // 创建并启动 Kafka 消费线程
                //consumerThreads_.emplace_back(&AsyncKafkaConsumer::consumeMessages, this, consumer);
                poolKafka.setThreadCount(4);
                //poolKafka.enqueue(&AsyncKafkaConsumer::consumeMessages, this, consumer);
                // 替换为 lambda 更安全、现代
                poolKafka.enqueue([this, consumer]() {
                    this->consumeMessages(consumer);
                });
            }
            LOG_INFO << "AsyncKafkaConsumerOne consumer started.";
        }
        catch (const std::exception& e)
        {
            LOG_ERROR << "AsyncKafkaConsumerOne Exception in constructor: " << e.what();
            // 可能需要进一步处理异常，例如重新尝试初始化
        }
    }

    ~AsyncKafkaConsumerOne()
    {
        stop_ = true;
        for (const auto consumer : consumers_)
        {
            rd_kafka_consumer_close(consumer);
            rd_kafka_flush(consumer, 1000);
            rd_kafka_destroy(consumer);
        }
        LOG_INFO << "AsyncKafkaConsumerOne consumer stopped.";
    }

private:
    void consumeMessages(rd_kafka_t* consumer_) const
    {
        //const auto consumer_ = KafkaManager::instance().getConsumer();
        // 创建一个新的 topic partition list
        rd_kafka_topic_partition_list_t *partitions = rd_kafka_topic_partition_list_new(1);
        // 向列表中添加主题和分区
        rd_kafka_topic_partition_list_add(partitions, "message_topic_one", RD_KAFKA_PARTITION_UA);
        // 订阅主题
        const rd_kafka_resp_err_t err = rd_kafka_subscribe(consumer_, partitions);
        // 销毁 topic partition list
        rd_kafka_topic_partition_list_destroy(partitions);

        if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            LOG_ERROR << "Failed to subscribe to topic: " << rd_kafka_err2str(err);
            return;
        }
        while (!stop_)
        {
            if (rd_kafka_message_t *msg = rd_kafka_consumer_poll(consumer_, 1000)) // Poll every second
            {
                // 启动协程处理消息
                async_run([msg, consumer_, this]() -> Task<> {
                    try
                    {
                        if (msg->err)
                        {
                            if (msg->err == RD_KAFKA_RESP_ERR__PARTITION_EOF)
                            {
                                // 当前分区的消息已经消费完毕
                                LOG_ERROR << "AsyncKafkaConsumerOne EReached end of partition.";
                            }
                            else
                            {
                                LOG_ERROR << "AsyncKafkaConsumerOne Error consuming message: " <<  rd_kafka_err2str(msg->err);
                            }
                            rd_kafka_message_destroy(msg); // 释放消息资源
                            co_return;
                        }

                        const std::string message(static_cast<const char*>(msg->payload), msg->len);

                        //LOG_INFO << "收到消息 Received message: " << message;
                        co_await handleKafkaMessage(message);

                        // 处理完消息后手动提交偏移量
                        rd_kafka_commit_message(consumer_, msg, 0);
                    }
                    catch (const std::exception& ex)
                    {
                        LOG_ERROR << "AsyncKafkaConsumerOne Exception while processing message: " << ex.what();
                    }
                    rd_kafka_message_destroy(msg); // 释放消息资源
                });
            }
        }
    }

    // 将处理 Kafka 消息的逻辑封装为协程
    static Task<> handleKafkaMessage(const std::string& message) {
        try {
            LOG_INFO << "AsyncKafkaConsumerOne 收到消息 Received message: " << message;
        } catch (const std::exception& ex) {
            throw std::invalid_argument(ex.what());
        }
        co_return;
    }

    std::vector<rd_kafka_t*> consumers_;
    std::atomic<bool> stop_{false};
};