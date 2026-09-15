//
// Created by 神圣•凯莎 on 24-7-30.
//

#pragma once
#include <librdkafka/rdkafka.h>
#include <atomic>
#include <cassert>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <drogon/utils/coroutine.h>
#include "kafka/KafkaManager.h"
#include "coroutinePool/TbbCoroutinePool.h"

class AsyncKafkaConsumer
{
public:
    AsyncKafkaConsumer(std::vector<std::string> topics,
                       std::function<drogon::Task<>(const std::string&)> handler,
                       const size_t numThreads = std::thread::hardware_concurrency())
        : stop_(false), topics_(std::move(topics)), messageHandler_(std::move(handler))
    {
        try
        {
            initializeConsumers(numThreads);
            LOG_DEBUG << "AsyncKafkaConsumer consumer started with " << numThreads << " threads.";
        }
        catch (const std::exception& e)
        {
            LOG_ERROR << "AsyncKafkaConsumer initialization failed: " << e.what();
            throw;
        }
    }

    ~AsyncKafkaConsumer()
    {
        requestStop();
        consumers_.clear();
        LOG_DEBUG << "AsyncKafkaConsumer consumer stopped.";
    }

    // 停掉 poll 线程并把 TBB 池里的在途任务排空。**幂等**，可被析构调用，
    // 也可被「进程退出钩子」提前调用（见 aop/Application.h）。
    //
    // ⚠️ 为什么必须能提前调用：drogon 在监听失败等致命错误里会直接 `exit(1)`
    //    （trantor Socket::bind → LOG_SYSERR + exit(1)）。`exit()` 会跑静态析构
    //    但**不等待其他线程**，于是「poll 线程还在往 TBB 投递 + TBB worker 正在
    //    `LOG_*`」与「日志器被静态析构」并发发生 —— 日志器 mutex 已失效，
    //    `~Logger()` → `AsyncFileLogger::flush()` 抛 `std::system_error(EINVAL)`，
    //    而 TBB worker 没有异常处理器 ⇒ `std::terminate` ⇒ SIGABRT(134)。
    //    实测：端口被占时秒崩，崩溃报告里能同时看到
    //    `~AsyncKafkaConsumerOne` 正在 `thread::join` 和 TBB worker 正在 terminate。
    void requestStop()
    {
        stop_ = true;
        for (auto& thread : pollThreads_)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
        // ⚠️ 必须等 TBB 池里的【在途任务】跑完，再释放 consumer。
        //
        // submitMessageTask 提交的 lambda 捕获的是裸 rd_kafka_t* consumer，
        // 任务体里还会调用 rd_kafka_commit_message(consumer, msg, 0) 与
        // rd_kafka_message_destroy(msg)。原实现只 join 了 poll 线程就
        // consumers_.clear()，于是「进程退出时刚好有消息在处理」这条路径上，
        // 在途任务会访问已经销毁的 consumer → use-after-free。
        // waitAll() 保证这些任务全部结束后才轮到 consumer 析构。
        TbbCoroutinePool::instance().waitAll();
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

            pollThreads_.emplace_back([this, consumer] {
                this->consumeMessages(consumer);
            });
        }
    }

    void submitMessageTask(rd_kafka_message_t* msg, rd_kafka_t* consumer)
    {
        if (!msg) return;

        TbbCoroutinePool::instance().submit([msg, consumer, this]() -> drogon::AsyncTask {
            try
            {
                if (msg->err)
                {
                    if (msg->err == RD_KAFKA_RESP_ERR__PARTITION_EOF)
                    {
                        LOG_DEBUG << "AsyncKafkaConsumer Reached end of partition.";
                    }
                    else
                    {
                        LOG_ERROR << "AsyncKafkaConsumer Error consuming message: " <<  rd_kafka_err2str(msg->err);
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
                LOG_ERROR << "AsyncKafkaConsumer Exception while processing message: " << ex.what();
            }
            catch (...)
            {
                ++stats_.errCount;
                LOG_ERROR << "AsyncKafkaConsumer Unknown exception while processing message";
            }
            rd_kafka_message_destroy(msg);
        });
    }

    void consumeMessages(rd_kafka_t* consumer_)
    {
        while (!stop_)
        {
            if (rd_kafka_message_t* msg = rd_kafka_consumer_poll(consumer_, 100))
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
        }
    }

    std::vector<std::unique_ptr<rd_kafka_t, KafkaDeleter>> consumers_; // Kafka 消费者实例
    std::vector<std::thread> pollThreads_;
    std::atomic<bool> stop_{false}; // 控制消费线程的停止

    std::vector<std::string> topics_;
    std::function<drogon::Task<>(const std::string&)> messageHandler_;
    struct KafkaStats {
        std::atomic<size_t> msgCount{0};
        std::atomic<size_t> errCount{0};
    } stats_;
};
