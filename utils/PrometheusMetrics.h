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
           << "ws_messages_received_total " << wsTotalMessages_.load(std::memory_order_relaxed) << "\n\n";

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
};

} // namespace Metrics
