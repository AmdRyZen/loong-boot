#pragma once
//
// 演示 / 调试输出的统一开关（默认关闭）。
//
// 为什么需要它：`std::cout` 完全不受 `config.json` 的 `log_level` 约束，
// 所以哪怕把日志级别调成 ERROR，请求路径上的这些打印照样会跑，而且
// 绝大多数用 `std::endl` —— 每次强制 flush。实测最热的
// `GET /api/v1/openapi/getValue` 单请求要打 5 条 cout，
// 在 173K QPS 下就是 ~86 万次/秒的无谓 flush 系统调用。
//
// 用法：把请求路径上的 `std::cout` 换成 `LOONG_DEMO_OUT`。
//   LOONG_DEMO_OUT << "x = " << x << std::endl;
// 默认静默；需要看演示输出时：
//   LOONG_VERBOSE=1 ./loong-boot
//
// 只用于「演示 / 调试」输出。真正的错误一律走 LOG_ERROR，不受本开关影响。
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace loong::log
{
// 进程内只求值一次（getenv 不便宜，不该出现在请求路径上）
inline bool verbose() noexcept
{
    static const bool enabled = []() noexcept {
        const char* v = std::getenv("LOONG_VERBOSE");
        if (v == nullptr || *v == '\0')
        {
            return false;
        }
        return std::string_view(v) != "0";
    }();
    return enabled;
}
} // namespace loong::log

#define LOONG_DEMO_OUT if (::loong::log::verbose()) std::cout
