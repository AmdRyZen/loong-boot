//
// Created by 神圣·凯莎 on 2022/6/24.
//

#ifndef DROGON_HTTP_CHECKLOGINUTILS_H
#define DROGON_HTTP_CHECKLOGINUTILS_H

#include <drogon/HttpAppFramework.h>
#include <jwt-cpp/jwt.h>
#include <optional>

#include "utils/RateLimitedLog.h"

class checkloginUtils
{
  public:
    [[gnu::always_inline]] inline static std::optional<std::string> checklogin(const drogon::HttpRequestPtr& req);
};

std::optional<std::string> checkloginUtils::checklogin(const drogon::HttpRequestPtr& req)
{
    try
    {
        const auto req_token = req->getHeader("Authorization");

        const auto decoded = jwt::decode(req_token);
        const auto verifier = jwt::verify()
                            .allow_algorithm(jwt::algorithm::hs256{drogon::app().getCustomConfig()["jwt-secret"].asString()})
                            .with_issuer("auth0");
        verifier.verify(decoded);
        return decoded.get_payload_claim("user_id").as_string();
    }
    catch (const std::exception& e)
    {
        // 触发频率由客户端决定：乱发 token 就能让每个请求写一行 ERROR。
        // 是真错误（鉴权失败）所以不静默，但必须限流。
        // 函数内 static：inline 函数保证全程序一份实例。
        static loong::log::RateLimiter limiter{1000};
        if (limiter.allow())
        {
            LOG_ERROR << "checklogin err = " << e.what() << " login: "
                      << (1 + limiter.takeSuppressed()) << " occurrence(s) since last log";
        }
        return std::nullopt;
    }
}
#endif  // DROGON_HTTP_CHECKLOGINUTILS_H
