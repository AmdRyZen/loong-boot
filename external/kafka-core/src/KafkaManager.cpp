// ===== File: src/KafkaManager.cpp =====
#include "kafka/KafkaManager.h"
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <ranges>

using namespace kafka;

namespace {

// ── 极简「每窗口最多一条」限流器 ─────────────────────────────────────────────
//
// 为什么 kafka-core 自带一份而不是用上层的 loong::log::RateLimiter：
// 本库是独立编译安装的静态库，不依赖上层工程的头文件（否则每次改上层日志
// 工具都要重装库）。语义与上层保持一致：窗口内第一条放行，其余只计数。
//
// CAS 抢时间戳，天然去重，无锁 —— 它会被 librdkafka 的多个内部线程并发调用。
struct RateLimiter
{
    std::atomic<int64_t> lastMs{0};
    std::atomic<uint64_t> sinceLastTake{0};
    std::atomic<uint64_t> total{0};

    bool allow(int64_t windowMs = 1000) noexcept
    {
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        int64_t last = lastMs.load(std::memory_order_relaxed);
        if (now - last < windowMs)
        {
            sinceLastTake.fetch_add(1, std::memory_order_relaxed);
            total.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (lastMs.compare_exchange_strong(last, now, std::memory_order_relaxed))
        {
            return true;
        }
        sinceLastTake.fetch_add(1, std::memory_order_relaxed);
        total.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    uint64_t takeSuppressed() noexcept
    {
        return sinceLastTake.exchange(0, std::memory_order_relaxed);
    }
};

// 所有「会被故障放大」的日志共用一把尺子：librdkafka 自身日志 + 投递失败报告。
// 分成两把的话，两者各自限流、合起来仍可能把日志冲垮。
RateLimiter g_logLimiter;

// 投递失败累计（唯一能反映 broker 不可达的计数，见头文件说明）。
std::atomic<uint64_t> g_deliveryFailed{0};

}  // namespace

KafkaManager::KafkaManager() : producer_conf_(nullptr), producer_(nullptr), consumer_conf_(nullptr), initialized_(false), destroyed_(false), logCallback_(nullptr) {}
KafkaManager::~KafkaManager() { stop(); }
KafkaManager& KafkaManager::instance() {
    static KafkaManager mgr;
    return mgr;
}

uint64_t KafkaManager::deliveryFailedCount() noexcept
{
    return g_deliveryFailed.load(std::memory_order_relaxed);
}

uint64_t KafkaManager::suppressedLogCount() noexcept
{
    return g_logLimiter.total.load(std::memory_order_relaxed);
}

void KafkaManager::setLogCallback(std::function<void(const std::string&)> cb)
{
    logCallback_ = std::move(cb);
}

void KafkaManager::emitLog(const std::string& msg) noexcept
{
    // 限流在最前面：被压掉的连字符串拼接都不做（调用方已拼好，但至少不再输出）。
    if (!g_logLimiter.allow())
    {
        return;
    }
    // 行内带上「本次覆盖多少条」：信息不丢，只是合并。
    const std::string line =
        msg + " (" + std::to_string(1 + g_logLimiter.takeSuppressed()) +
        " occurrence(s) since last log, total suppressed: " +
        std::to_string(g_logLimiter.total.load(std::memory_order_relaxed)) + ")";
    try
    {
        if (instance().logCallback_)
        {
            instance().logCallback_(line);
            return;
        }
    }
    catch (...)
    {
        // 调用方的日志回调抛异常绝不能穿出去：这里跑在 librdkafka 的
        // poll / 内部线程上，异常穿出等于 terminate。
    }
    // 没有回调时的兜底：仍然是【限流后】的 stderr，绝不逐条输出。
    std::cerr << line << std::endl;
}

void KafkaManager::rdKafkaLogCallback(const rd_kafka_t* rk, int level, const char* fac, const char* buf)
{
    (void)rk;
    // level 是 syslog 语义（3=err 4=warning ... 7=debug）。
    // 只把 err 及更严重的转出去：warning/notice/info 在 broker 抖动时会刷屏
    // 而信息量极低（"1/1 brokers are down" 这类已经在 err 里覆盖了）。
    if (level > 3)
    {
        return;
    }
    try
    {
        std::string msg = "librdkafka[";
        msg += (fac != nullptr ? fac : "?");
        msg += "]: ";
        msg += (buf != nullptr ? buf : "");
        emitLog(msg);
    }
    catch (...)
    {
        // 同上：绝不让异常逃出回调
    }
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

        // ── 把 librdkafka 自身的日志接进我们的日志出口 ─────────────────────────
        //
        // 不装 log_cb 的话，librdkafka 直接把日志写 stderr：绕过调用方的日志
        // 系统（不落文件、不受 log_level 过滤、无法限流）。实测 broker 不可达时
        // 约 150 行/秒的 "1/1 brokers are down"，是故障期日志占盘的主因。
        //
        // log_level 显式设成 3(err)：默认是 6(info)，会把 warning/notice/info
        // 也送进回调 —— 在 broker 抖动时这些几乎全是噪声。
        rd_kafka_conf_set(producer_conf_, "log_level", "3", errStr, sizeof(errStr));
        rd_kafka_conf_set_log_cb(producer_conf_, KafkaManager::rdKafkaLogCallback);

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

        // ⚠️ 必须立刻置空：rd_kafka_new() 成功后【conf 的所有权已转移给 producer】。
        // librdkafka 头文件原文（rdkafka.h, rd_kafka_conf_t 说明）：
        //   "A successful call to rd_kafka_new() will assume ownership of the conf
        //    object and rd_kafka_conf_destroy() must not be called."
        // 不置空的话，stop() 里那句 rd_kafka_conf_destroy(producer_conf_) 就是对
        // 已释放内存的二次销毁 —— double free，每次进程退出必触发
        //（initialize 在 Application 启动回调里是无条件执行的，与落库开关无关）。
        // 失败路径不置空：那时 conf 仍归调用方所有，仍需在 stop() 里销毁。
        producer_conf_ = nullptr;

        consumer_conf_ = rd_kafka_conf_new();
        if (rd_kafka_conf_set(consumer_conf_, "bootstrap.servers", brokers.c_str(), errStr, sizeof(errStr)) != RD_KAFKA_CONF_OK)
            throw std::runtime_error(std::string("Consumer bootstrap.servers config error: ") + errStr);
        if (rd_kafka_conf_set(consumer_conf_, "group.id", groupId.c_str(), errStr, sizeof(errStr)) != RD_KAFKA_CONF_OK)
            throw std::runtime_error(std::string("Consumer group.id config error: ") + errStr);
        if (rd_kafka_conf_set(consumer_conf_, "enable.auto.commit", "false", errStr, sizeof(errStr)) != RD_KAFKA_CONF_OK)
            throw std::runtime_error(std::string("Consumer enable.auto.commit config error: ") + errStr);

        // 消费者同样接上日志出口。createNewConsumer() 走 rd_kafka_conf_dup()，
        // log_cb 与 log_level 都会被复制过去 —— 所以本机那 9 个 consumer
        //（两个 AsyncKafkaConsumer × 4 线程 + 若干）也一并被收敛，不必逐个改。
        rd_kafka_conf_set(consumer_conf_, "log_level", "3", errStr, sizeof(errStr));
        rd_kafka_conf_set_log_cb(consumer_conf_, KafkaManager::rdKafkaLogCallback);

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
            emitLog("Error: Attempted to get empty topic name.");
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

    bool KafkaManager::safeProduce(rd_kafka_topic_t* topic, const std::string& message,
                                   const std::string& key)
    {
        auto* payload = strdup(message.c_str());
        if (!payload)
        {
            return false;
        }

        // key 的内存不需要我们持有：librdkafka 在 rd_kafka_produce() 返回前用完
        //（rdkafka.h 原文："the memory backing the key or the topic name may be
        //  reused as soon as rd_kafka_produce() returns"）。所以直接传指针，
        // 不 strdup —— 少一次分配 + 少一次 free，且热路径上零额外开销。
        // 空 key 传 nullptr/0：librdkafka 语义上「无 key」会让分区器轮询分区。
        const void* keyPtr = key.empty() ? nullptr : static_cast<const void*>(key.data());
        const size_t keyLen = key.empty() ? 0 : key.size();

        if (rd_kafka_produce(
                topic,
                RD_KAFKA_PARTITION_UA,
                RD_KAFKA_MSG_F_FREE,
                payload, message.size(),
                keyPtr, keyLen,
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
                    // 走统一出口（限流）：poll 线程的异常若持续发生，逐条写 stderr
                    // 同样是「与故障时长成正比」的日志放大。
                    emitLog(std::string("Exception in poll thread: ") + e.what());
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
        (void)rk;
        (void)opaque;
        if (rkmessage->err) {
            // ── 计数优先，日志次之 ────────────────────────────────────────────
            //
            // 这是【唯一】能反映「broker 不可达」的信号：safeProduce 返回 true
            // 只代表消息进了 librdkafka 本地队列，broker 挂掉时每条都返回 true，
            // 真失败要等 delivery.timeout.ms（默认 30s）之后才走到这里。
            // 实测：bootstrap.servers 指向死地址 + 发 300 条 ⇒ produce 失败计数
            // 恒 0，而本回调报了 302 次。所以计数必须在这里累加。
            g_deliveryFailed.fetch_add(1, std::memory_order_relaxed);

            // ⚠️ 绝不再逐条写 stderr。
            //
            // 原实现是 `std::cerr << "Message delivery failed: ..." << std::endl;`
            // 一条失败一行，无节流、且绕过日志系统（不落 log/loong.log、不受
            // log_level 控制）。broker 挂掉时它的触发频率与【消息速率】成正比：
            // 实测 300 条消息报了 302 行。
            //
            // 更麻烦的是：本回调运行在 rd_kafka_poll 内部（poll 线程上），
            // 回调变慢会直接卡住 poll —— 所以这里只做「计数 + 限流后的一行」。
            std::string msg = "Message delivery failed: ";
            msg += rd_kafka_err2str(rkmessage->err);
            if (rkmessage->rkt != nullptr) {
                msg += ", topic: ";
                msg += rd_kafka_topic_name(rkmessage->rkt);
            }
            msg += ", total: ";
            msg += std::to_string(g_deliveryFailed.load(std::memory_order_relaxed));
            emitLog(msg);
        } else {
            /*std::cout << "Message delivered to topic " << rd_kafka_topic_name(rkmessage->rkt)
                      << " [" << rkmessage->partition << "] at offset " << rkmessage->offset << std::endl;*/
        }
    }
