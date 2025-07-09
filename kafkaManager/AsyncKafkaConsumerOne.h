//
// Created by 神圣•凯莎 on 24-7-30.
//

#pragma once
#include <rdkafka.h>
#include <atomic>
#include <cassert>
#include <limits>
#include <memory>
#include <functional>
#include "kafka/KafkaManager.h"
#include "coroutinePool/TbbCoroutinePool.h"

using namespace drogon;
class AsyncKafkaConsumerOne
{
public:
    AsyncKafkaConsumerOne(std::vector<std::string> topics,
                       std::function<Task<>(const std::string&)> handler,
                       const size_t numThreads = std::thread::hardware_concurrency())
        : stop_(false), topics_(std::move(topics)), messageHandler_(std::move(handler))
    {
        try
        {
            initializeConsumers(numThreads);
            LOG_INFO << "AsyncKafkaConsumerOne consumer started with " << numThreads << " threads.";
        }
        catch (const std::exception& e)
        {
            LOG_ERROR << "AsyncKafkaConsumerOne initialization failed: " << e.what();
            throw;
        }
    }

    ~AsyncKafkaConsumerOne()
    {
        stop_ = true;
        consumers_.clear();
        LOG_INFO << "AsyncKafkaConsumerOne consumer stopped.";
    }

private:
    struct KafkaDeleter {
        void operator()(rd_kafka_t* c) const {
            rd_kafka_consumer_close(c);
            rd_kafka_flush(c, 1000);
            rd_kafka_destroy(c);
        }
    };

    void initializeConsumers(const size_t numThreads)
    {
        for (size_t i = 0; i < numThreads; ++i)
        {
            rd_kafka_t* consumer = kafka::KafkaManager::instance().createNewConsumer();
            if (!consumer)
            {
                throw std::runtime_error("Failed to create Kafka consumer.");
            }

            // 订阅 topic
            assert(topics_.size() <= static_cast<size_t>(std::numeric_limits<int>::max()));
            rd_kafka_topic_partition_list_t *partitions = rd_kafka_topic_partition_list_new(static_cast<int>(topics_.size()));
            for (const auto& topic : topics_) {
                rd_kafka_topic_partition_list_add(partitions, topic.c_str(), RD_KAFKA_PARTITION_UA);
            }
            const rd_kafka_resp_err_t err = rd_kafka_subscribe(consumer, partitions);
            rd_kafka_topic_partition_list_destroy(partitions);

            if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
            {
                throw std::runtime_error(std::string("Kafka consumer subscription failed: ") + rd_kafka_err2str(err));
            }

            consumers_.emplace_back(consumer);

            // 消费线程任务入队，改用 TbbCoroutinePool
            TbbCoroutinePool::instance().submit([this, consumer]() -> AsyncTask {
                this->consumeMessages(consumer);
                co_return;
            });
        }
    }

    void submitMessageTask(rd_kafka_message_t* msg, rd_kafka_t* consumer)
    {
        TbbCoroutinePool::instance().submit([msg, consumer, this]() -> AsyncTask {
            try
            {
                if (msg->err)
                {
                    if (msg->err == RD_KAFKA_RESP_ERR__PARTITION_EOF)
                    {
                        LOG_ERROR << "AsyncKafkaConsumerOne Reached end of partition.";
                    }
                    else
                    {
                        LOG_ERROR << "AsyncKafkaConsumerOne Error consuming message: " <<  rd_kafka_err2str(msg->err);
                    }
                    ++stats_.errCount;
                    rd_kafka_message_destroy(msg);
                    co_return;
                }

                const std::string message(static_cast<const char*>(msg->payload), msg->len);
                ++stats_.msgCount;
                co_await messageHandler_(message);
                rd_kafka_commit_message(consumer, msg, 0);
            }
            catch (const std::exception& ex)
            {
                ++stats_.errCount;
                LOG_ERROR << "AsyncKafkaConsumerOne Exception while processing message: " << ex.what();
            }
            rd_kafka_message_destroy(msg);
        });
    }

    void consumeMessages(rd_kafka_t* consumer_)
    {
        while (!stop_)
        {
            if (rd_kafka_message_t* msg = rd_kafka_consumer_poll(consumer_, 50))
            {
                constexpr int maxBatchSize = 32;
                submitMessageTask(msg, consumer_);

                for (int i = 1; i < maxBatchSize && !stop_; ++i)
                {
                    rd_kafka_message_t* nextMsg = rd_kafka_consumer_poll(consumer_, 0);
                    if (!nextMsg) break;

                    submitMessageTask(nextMsg, consumer_);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    std::vector<std::unique_ptr<rd_kafka_t, KafkaDeleter>> consumers_; // Kafka 消费者实例
    std::atomic<bool> stop_{false}; // 控制消费线程的停止

    std::vector<std::string> topics_;
    std::function<Task<>(const std::string&)> messageHandler_;
    struct KafkaStats {
        std::atomic<size_t> msgCount{0};
        std::atomic<size_t> errCount{0};
    } stats_;
};