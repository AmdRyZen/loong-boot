#pragma once
#include <string>
#include <string_view>
#include <atomic>
#include <random>
#include <chrono>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

namespace Trace {

// 全局 TraceId 上下文 Key
inline constexpr const char* TRACE_HEADER = "X-Trace-Id";

class TraceContext {
public:
    // 生成全局高性能唯一 TraceId: 纳秒时间戳(16进制) + 进程计数器 + 随机数
    static std::string generateTraceId() {
        static std::atomic<uint32_t> counter{0};
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const uint32_t seq = counter.fetch_add(1, std::memory_order_relaxed);

        char buf[64];
        const int len = std::snprintf(buf, sizeof(buf), "%llx-%04x-%04x",
                                      static_cast<unsigned long long>(now),
                                      seq & 0xFFFF,
                                      static_cast<uint32_t>(rand() & 0xFFFF));
        return std::string(buf, len);
    }

    // 从请求中提取或自动注入 TraceId
    static std::string getOrCreateTraceId(const drogon::HttpRequestPtr& req) {
        if (!req) return generateTraceId();

        // 优先读取客户端传入的 Header
        const auto existing = req->getHeader(TRACE_HEADER);
        if (!existing.empty()) {
            return existing;
        }

        // 没有则新生成
        return generateTraceId();
    }
};

} // namespace Trace
