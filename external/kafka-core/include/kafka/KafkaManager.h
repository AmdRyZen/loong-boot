// ===== File: include/kafka/KafkaManager.h =====
#ifndef KAFKA_CORE_KAFKA_MANAGER_H
#define KAFKA_CORE_KAFKA_MANAGER_H

#pragma once

#include <librdkafka/rdkafka.h>
#include <string>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <cstdint>
#include <functional>

namespace kafka {

class KafkaManager {
public:
    static KafkaManager& instance();

    // ── 日志出口 ─────────────────────────────────────────────────────────────
    //
    // 必须在 initialize() 【之前】设置（之后不再改动，回调路径上不加锁读取）。
    //
    // 为什么需要：默认情况下 librdkafka 【没有】log_cb，它会把日志直接写
    // stderr —— 绕过调用方的日志系统：不落日志文件、不受 log_level 过滤、
    // 也无法限流。实测 broker 不可达时（9 个 consumer 各自刷
    // "1/1 brokers are down"）约 150 行/秒，53 秒 7808 行，是 Kafka 故障时
    // 日志占盘的主因。
    //
    // 设置后，librdkafka 自身日志与投递失败报告都会走这个回调（已限流），
    // 调用方可以接进自己的日志系统。
    void setLogCallback(std::function<void(const std::string&)> cb);

    void initialize(const std::string& brokers, const std::string& groupId = "default_group");
    bool isHealthy() const;
    rd_kafka_t* getProducer() const;
    rd_kafka_t* createNewConsumer() const;
    rd_kafka_topic_t* getTopic(const std::string& topicName);

    // ── 生产一条消息 ─────────────────────────────────────────────────────────
    //
    // key 可选：非空时作为分区键（相同 key 落到同一分区 ⇒ 该 key 的消息在
    // 分区内保序 —— 这是「按房间回放历史消息」能保序的前提；没有 key 时
    // librdkafka 轮询分区，同一个房间的消息会散到不同分区，回放必然乱序）。
    //
    // ⚠️ key 的内存【不需要】由调用方保持有效：librdkafka 会在
    //    rd_kafka_produce() 返回前把它用完（与 payload 的 RD_KAFKA_MSG_F_FREE
    //    语义不同，见 rdkafka.h 里 "the memory backing the key or the topic name
    //    may be reused as soon as rd_kafka_produce() returns"）。
    //    所以这里直接传引用，不做 strdup —— 少一次分配 + 少一次 free。
    static bool safeProduce(rd_kafka_topic_t* topic, const std::string& message,
                            const std::string& key = std::string());

    // ── 「broker 不可达」的唯一可靠计数 ───────────────────────────────────────
    //
    // safeProduce 返回 true 只代表消息进了 librdkafka 本地队列。broker 挂掉时
    // 每条都返回 true，真失败要等 delivery.timeout.ms（默认 30s）之后才由
    // 投递报告回调报出来 —— 所以「produce 失败数为 0」不代表 Kafka 正常，
    // 必须看这个数。（实测：broker 指向死地址 + 发 300 条，produce 失败计数恒 0，
    // 而投递报告回调报了 302 次。）
    static uint64_t deliveryFailedCount() noexcept;

    // 被限流器压掉的日志行数（librdkafka 自身日志 + 投递失败日志共用一把尺子）。
    // 用来确认「日志没有静默」：被压掉的是同一类事件的重复，不是新信息。
    static uint64_t suppressedLogCount() noexcept;

    void stop();

    KafkaManager(const KafkaManager&) = delete;
    KafkaManager& operator=(const KafkaManager&) = delete;

private:
    KafkaManager();
    ~KafkaManager();
    void startPolling();
    void stopPolling();
    static void deliveryReportCallback(rd_kafka_t* rk, const rd_kafka_message_t* rkmessage, void* opaque);
    // librdkafka 自身的日志回调（装在 conf 上）。注意它在 librdkafka 的
    // 内部线程里被调用，不在我们的 poll 线程上。
    static void rdKafkaLogCallback(const rd_kafka_t* rk, int level, const char* fac, const char* buf);
    // 限流后的统一日志出口：走 logCallback_，未设置时退回【已限流的】stderr。
    // 无论调用方有没有设回调，都不会出现「每条消息一行」的放大。
    static void emitLog(const std::string& msg) noexcept;

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
    // ⚠️ 生命周期约定：只能在 initialize() 之前赋值（调用方在进程启动期设置）。
    // 之后投递报告回调会在 librdkafka 线程上读它，因此【不能】再加锁改 ——
    // 那会在 poll 线程里引入锁竞争，而投递回调本来就跑在 rd_kafka_poll 内部。
    std::function<void(const std::string&)> logCallback_;
};

} // namespace kafka

#endif // KAFKA_CORE_KAFKA_MANAGER_H
