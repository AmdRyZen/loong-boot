#pragma once
#include <atomic>
#include <string>
#include <sstream>
#include <chrono>
#include <drogon/HttpAppFramework.h>
#include "coroutinePool/TbbCoroutinePool.h"

#if defined(__has_include)
  #if __has_include(<mimalloc.h>)
    #include <mimalloc.h>
    #define HAS_MIMALLOC 1
  #endif
#endif

namespace Metrics {

class PrometheusRegistry {
public:
    static PrometheusRegistry& instance() {
        static PrometheusRegistry registry;
        return registry;
    }

    // HTTP 请求计数与状态统计
    void recordHttpRequest(const std::string& method, int statusCode, double durationMs) {
        httpTotalRequests_.fetch_add(1, std::memory_order_relaxed);
        if (statusCode >= 200 && statusCode < 400) {
            http2xx3xxRequests_.fetch_add(1, std::memory_order_relaxed);
        } else if (statusCode >= 400 && statusCode < 500) {
            http4xxRequests_.fetch_add(1, std::memory_order_relaxed);
        } else if (statusCode >= 500) {
            http5xxRequests_.fetch_add(1, std::memory_order_relaxed);
        }
        totalLatencyMs_.fetch_add(static_cast<uint64_t>(durationMs * 1000), std::memory_order_relaxed);
    }

    // WebSocket 统计
    void recordWsConnect() {
        wsOnlineConnections_.fetch_add(1, std::memory_order_relaxed);
    }

    void recordWsDisconnect() {
        wsOnlineConnections_.fetch_sub(1, std::memory_order_relaxed);
    }

    void recordWsMessage(uint64_t count = 1) {
        wsTotalMessages_.fetch_add(count, std::memory_order_relaxed);
    }

    // 因【房间分片积压达上限】而被丢弃的消息数（含聊天消息与入群/退群公告）。
    // 该计数必须暴露：一旦持续增长说明吞吐已达上限，需要扩实例而不是继续加压。
    //
    // ⚠️ 语义收窄（2026-09-12）：此前 Kafka 持久化的 TBB 池饱和也往这里 +1，
    // 于是「消息没投出去」和「消息投出去了但没落库」混成同一个数，线上看到涨了
    // 无法判断是哪一类。现在这里只表示前者，后者见 recordKafkaPersistDropped()。
    void recordWsMessageDropped(uint64_t count = 1) {
        wsDroppedMessages_.fetch_add(count, std::memory_order_relaxed);
    }

    // 因 TBB 协程池饱和而被丢弃的 Kafka 持久化次数。
    //
    // 与 recordWsMessageDropped() 的区别：这条消息【已经投递给订阅者了】，
    // 只是没能异步落库 —— 丢的是历史记录，不是实时消息。
    // 判读：持续增长说明 TBB 池长期打满（Kafka broker 慢 / 分区数不足），
    //       实时链路仍正常，但历史回放会缺数据。
    void recordKafkaPersistDropped(uint64_t count = 1) {
        wsKafkaPersistDropped_.fetch_add(count, std::memory_order_relaxed);
    }

    // WebSocket 回调里被 catch 吞掉的异常次数。
    //
    // 为什么需要：这几处 catch 原先只写一行 LOG_ERROR。异常是罕见事件，
    // 但一旦发生（分配失败、序列化异常等）线上没有任何可累加的信号，
    // 只能去翻日志 —— 而日志可能被级别过滤或轮转掉。
    // 判读：非 0 即应查日志定位；具体是哪个 handler 看日志里的前缀。
    void recordWsHandlerException(uint64_t count = 1) {
        wsHandlerExceptions_.fetch_add(count, std::memory_order_relaxed);
    }

    // 因空闲超时（60 秒未收到客户端任意帧）被服务端主动断开的连接数。
    // 正常客户端会被 drogon 的协议层 Ping（默认 30s）+ 自动 Pong 保活，
    // 因此该计数持续增长意味着客户端异常掉线（进程被杀 / 网络中断）而非正常退出。
    void recordWsEvictedIdle(uint64_t count = 1) {
        wsEvictedIdle_.fetch_add(count, std::memory_order_relaxed);
    }

    // 过载通知（503「消息未投递」）被限流抑制的次数。
    //
    // 为什么必须限流：饱和时丢弃是 O(丢弃数) 的。若每条丢弃都回一帧通知，
    // 丢弃路径的成本就和真实投递一样高 —— 实测 30s 回声饱和压测中
    // 14.36M 次丢弃曾产生 14.36M 次额外 send。现在按连接限流（最快 1 秒一条），
    // 被抑制的部分记在这里。
    // 判读：该值快速增长说明服务端确实在持续过载，应扩实例；
    //       它本身不是错误，而是「已丢弃但未逐条告知」的差额。
    void recordWsOverloadNoticeSuppressed(uint64_t count = 1) {
        wsOverloadNoticeSuppressed_.fetch_add(count, std::memory_order_relaxed);
    }

    // 客户端发来的负载无法解析的次数。
    // 该计数必须暴露：触发频率由客户端决定，持续增长说明有人在发非法负载
    // （日志已按秒限流，所以「日志里看不到」不等于「没发生」，要看这个数）。
    void recordWsJsonParseError(uint64_t count = 1) {
        wsJsonParseErrors_.fetch_add(count, std::memory_order_relaxed);
    }

    // 房间侧快照（由 ChatWebsocket 的 5 秒定时任务推送）。
    // 这里存的是上一次采样的值：/metrics 抓取时无需再去加房间表的锁，
    // 代价是最多 5 秒的滞后 —— 对 gauge 类指标完全够用。
    void setRoomStats(uint64_t rooms,
                      uint64_t shards,
                      uint64_t subscribers,
                      uint64_t maxRoomSubscribers) {
        wsRoomsActive_.store(rooms, std::memory_order_relaxed);
        wsRoomShards_.store(shards, std::memory_order_relaxed);
        wsRoomSubscribers_.store(subscribers, std::memory_order_relaxed);
        wsRoomMaxSubscribers_.store(maxRoomSubscribers, std::memory_order_relaxed);
    }

    // 扇出分组效果（A1 优化的效果观测，同样由 5 秒定时任务推送）。
    //
    // 判读：一次扇出共 M 条消息、N 个订阅者、K 个 IO 线程时
    //   快速路径生效 → crossthreadBatches 增量 ≈ M×K，inloopDeliveries 增量 ≈ M×N；
    //   分组没生效（loop 抓取失败 / 回退直投）→ crossthreadBatches 会与 M×N 同量级，
    //   而 inloopDeliveries 增量接近 0。
    // 两个数都是 counter，看增量而不是绝对值。
    // crossthreadBatches 是「唤醒次数」，crossthreadDeliveries 是「份数」。
    // 两者必须分开记：只看 batches 无法还原实际送达份数（batch 里裹了几个连接
    // 是运行时才知道的），线上就少了一个「实际投递总量」的判据。
    void setFanoutStats(uint64_t inloopDeliveries,
                        uint64_t crossthreadBatches,
                        uint64_t crossthreadDeliveries) {
        wsFanoutInLoop_.store(inloopDeliveries, std::memory_order_relaxed);
        wsFanoutCrossThread_.store(crossthreadBatches, std::memory_order_relaxed);
        wsFanoutCrossThreadDeliveries_.store(crossthreadDeliveries, std::memory_order_relaxed);
    }

    // 运行期开关的当前值（0/1），由 ChatWebsocket::reloadSwitches() 每 5 秒推送。
    //
    // 为什么必须有：开关现在支持热更新，那就必须能看见「此刻到底是开还是关」——
    // 否则又回到「改了配置到底生效没有」的盲区（原实现是 static const，
    // 只在首次调用求值一次，改配置永远不生效且没有任何提示）。
    void setSwitchStates(bool kafkaPersistence) {
        wsKafkaPersistenceEnabled_.store(kafkaPersistence ? 1 : 0, std::memory_order_relaxed);
    }

    // 生成标准 Prometheus 文本格式导出
    std::string exportPrometheusText() const {
        std::ostringstream ss;
        const auto reqs = httpTotalRequests_.load(std::memory_order_relaxed);
        const auto totalLatUs = totalLatencyMs_.load(std::memory_order_relaxed);
        const double avgLatMs = reqs > 0 ? (static_cast<double>(totalLatUs) / 1000.0 / reqs) : 0.0;

        // 系统与运行时基础信息
        ss << "# HELP loong_boot_uptime_seconds Process uptime in seconds.\n"
           << "# TYPE loong_boot_uptime_seconds gauge\n"
           << "loong_boot_uptime_seconds " << getUptimeSeconds() << "\n\n";

        // TBB 协程线程池指标 (核心防爆监控)
        const size_t activeTbb = TbbCoroutinePool::instance().getActiveTasks();
        ss << "# HELP tbb_coroutine_pool_active_tasks In-flight task count in TBB pool.\n"
           << "# TYPE tbb_coroutine_pool_active_tasks gauge\n"
           << "tbb_coroutine_pool_active_tasks " << activeTbb << "\n"
           << "tbb_coroutine_pool_capacity 32768\n\n";

        // WebSocket 实时度量
        ss << "# HELP ws_connections_current Number of current active WebSocket connections.\n"
           << "# TYPE ws_connections_current gauge\n"
           << "ws_connections_current " << wsOnlineConnections_.load(std::memory_order_relaxed) << "\n\n"
           << "# HELP ws_messages_received_total Total WebSocket messages processed.\n"
           << "# TYPE ws_messages_received_total counter\n"
           << "ws_messages_received_total " << wsTotalMessages_.load(std::memory_order_relaxed) << "\n\n"
           << "# HELP ws_messages_dropped_total WebSocket messages dropped due to room shard backlog.\n"
           << "# TYPE ws_messages_dropped_total counter\n"
           << "ws_messages_dropped_total " << wsDroppedMessages_.load(std::memory_order_relaxed) << "\n\n"
           << "# HELP ws_kafka_persist_dropped_total Kafka persistence skipped because the TBB pool was saturated (message was still delivered live).\n"
           << "# TYPE ws_kafka_persist_dropped_total counter\n"
           << "ws_kafka_persist_dropped_total " << wsKafkaPersistDropped_.load(std::memory_order_relaxed) << "\n\n"
           << "# HELP ws_handler_exceptions_total Exceptions caught and swallowed inside WebSocket callbacks (non-zero means check logs).\n"
           << "# TYPE ws_handler_exceptions_total counter\n"
           << "ws_handler_exceptions_total " << wsHandlerExceptions_.load(std::memory_order_relaxed) << "\n\n"
           << "# HELP ws_kafka_persistence_enabled Effective value of custom_config.enable_kafka_persistence (hot-reloaded every 5s).\n"
           << "# TYPE ws_kafka_persistence_enabled gauge\n"
           << "ws_kafka_persistence_enabled " << wsKafkaPersistenceEnabled_.load(std::memory_order_relaxed) << "\n\n"
           << "# HELP ws_evicted_idle_total WebSocket connections force-closed after the 60s idle timeout.\n"
           << "# TYPE ws_evicted_idle_total counter\n"
           << "ws_evicted_idle_total " << wsEvictedIdle_.load(std::memory_order_relaxed) << "\n\n"
           << "# HELP ws_overload_notice_suppressed_total Overload (503) notices suppressed by the per-connection rate limit.\n"
           << "# TYPE ws_overload_notice_suppressed_total counter\n"
           << "ws_overload_notice_suppressed_total " << wsOverloadNoticeSuppressed_.load(std::memory_order_relaxed) << "\n\n"
           << "# HELP ws_json_parse_errors_total Client messages that failed to parse (rate-limited in logs; watch this counter).\n"
           << "# TYPE ws_json_parse_errors_total counter\n"
           << "ws_json_parse_errors_total " << wsJsonParseErrors_.load(std::memory_order_relaxed) << "\n\n";

        // 房间注册表度量（由 5 秒定时任务推送，最多 5 秒滞后）
        ss << "# HELP ws_rooms_active Number of active chat rooms.\n"
           << "# TYPE ws_rooms_active gauge\n"
           << "ws_rooms_active " << wsRoomsActive_.load(std::memory_order_relaxed) << "\n"
           << "# HELP ws_room_shards_active Number of non-empty shards across all rooms.\n"
           << "# TYPE ws_room_shards_active gauge\n"
           << "ws_room_shards_active " << wsRoomShards_.load(std::memory_order_relaxed) << "\n"
           << "# HELP ws_room_subscribers_total Total subscribers across all rooms.\n"
           << "# TYPE ws_room_subscribers_total gauge\n"
           << "ws_room_subscribers_total " << wsRoomSubscribers_.load(std::memory_order_relaxed) << "\n"
           << "# HELP ws_room_max_subscribers Subscriber count of the largest room (fanout cost driver).\n"
           << "# TYPE ws_room_max_subscribers gauge\n"
           << "ws_room_max_subscribers " << wsRoomMaxSubscribers_.load(std::memory_order_relaxed) << "\n\n";

        // 扇出分组效果（A1）。两个都是 counter：看增量，不看绝对值。
        // 健康形态：crossthread_batches 增量 ≈ 消息数 × IO线程数，远小于
        // inloop_deliveries 增量；若前者与「消息数 × 订阅者数」同量级，说明分组没生效。
        ss << "# HELP ws_fanout_inloop_deliveries_total Fanout deliveries executed on the caller's own loop (no cross-thread hop).\n"
           << "# TYPE ws_fanout_inloop_deliveries_total counter\n"
           << "ws_fanout_inloop_deliveries_total " << wsFanoutInLoop_.load(std::memory_order_relaxed) << "\n"
           << "# HELP ws_fanout_crossthread_batches_total Fanout batches dispatched to another IO loop (one wakeup each).\n"
           << "# TYPE ws_fanout_crossthread_batches_total counter\n"
           << "ws_fanout_crossthread_batches_total " << wsFanoutCrossThread_.load(std::memory_order_relaxed) << "\n"
           << "# HELP ws_fanout_crossthread_deliveries_total Deliveries carried inside those cross-thread batches.\n"
           << "# TYPE ws_fanout_crossthread_deliveries_total counter\n"
           << "ws_fanout_crossthread_deliveries_total " << wsFanoutCrossThreadDeliveries_.load(std::memory_order_relaxed) << "\n"
           << "# HELP ws_fanout_deliveries_total Total successful fanout deliveries (in-loop + cross-thread).\n"
           << "# TYPE ws_fanout_deliveries_total counter\n"
           << "ws_fanout_deliveries_total "
           << (wsFanoutInLoop_.load(std::memory_order_relaxed) +
               wsFanoutCrossThreadDeliveries_.load(std::memory_order_relaxed))
           << "\n\n";

        // HTTP 度量
        ss << "# HELP http_requests_total Total HTTP requests handled.\n"
           << "# TYPE http_requests_total counter\n"
           << "http_requests_total{status=\"2xx_3xx\"} " << http2xx3xxRequests_.load(std::memory_order_relaxed) << "\n"
           << "http_requests_total{status=\"4xx\"} " << http4xxRequests_.load(std::memory_order_relaxed) << "\n"
           << "http_requests_total{status=\"5xx\"} " << http5xxRequests_.load(std::memory_order_relaxed) << "\n"
           << "http_requests_total " << reqs << "\n\n"
           << "# HELP http_request_duration_average_ms Average request latency in milliseconds.\n"
           << "# TYPE http_request_duration_average_ms gauge\n"
           << "http_request_duration_average_ms " << avgLatMs << "\n\n";

        // 内存分配器指标 (mimalloc)
#ifdef HAS_MIMALLOC
        size_t miReserved = 0;
        size_t miCommitted = 0;
        mi_process_info(&miReserved, &miCommitted, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        ss << "# HELP mimalloc_committed_bytes Committed memory tracked by mimalloc.\n"
           << "# TYPE mimalloc_committed_bytes gauge\n"
           << "mimalloc_committed_bytes " << miCommitted << "\n"
           << "mimalloc_reserved_bytes " << miReserved << "\n\n";
#endif

        return ss.str();
    }

private:
    PrometheusRegistry() : startTime_(std::chrono::steady_clock::now()) {}

    int64_t getUptimeSeconds() const {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startTime_).count();
    }

    std::chrono::steady_clock::time_point startTime_;
    std::atomic<uint64_t> httpTotalRequests_{0};
    std::atomic<uint64_t> http2xx3xxRequests_{0};
    std::atomic<uint64_t> http4xxRequests_{0};
    std::atomic<uint64_t> http5xxRequests_{0};
    std::atomic<uint64_t> totalLatencyMs_{0}; // 微秒累计
    std::atomic<int64_t> wsOnlineConnections_{0};
    std::atomic<uint64_t> wsTotalMessages_{0};
    std::atomic<uint64_t> wsDroppedMessages_{0};
    std::atomic<uint64_t> wsKafkaPersistDropped_{0};
    std::atomic<uint64_t> wsHandlerExceptions_{0};
    std::atomic<uint64_t> wsKafkaPersistenceEnabled_{0};
    std::atomic<uint64_t> wsEvictedIdle_{0};
    std::atomic<uint64_t> wsOverloadNoticeSuppressed_{0};
    std::atomic<uint64_t> wsJsonParseErrors_{0};
    std::atomic<uint64_t> wsRoomsActive_{0};
    std::atomic<uint64_t> wsRoomShards_{0};
    std::atomic<uint64_t> wsRoomSubscribers_{0};
    std::atomic<uint64_t> wsRoomMaxSubscribers_{0};
    std::atomic<uint64_t> wsFanoutInLoop_{0};
    std::atomic<uint64_t> wsFanoutCrossThread_{0};
    std::atomic<uint64_t> wsFanoutCrossThreadDeliveries_{0};
};

} // namespace Metrics
