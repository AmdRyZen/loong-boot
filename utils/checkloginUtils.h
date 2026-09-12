//
// Created by 神圣·凯莎 on 2022/6/24.
//

#ifndef DROGON_HTTP_CHECKLOGINUTILS_H
#define DROGON_HTTP_CHECKLOGINUTILS_H

#include <drogon/HttpAppFramework.h>
#include <jwt-cpp/jwt.h>
#include <optional>
#include <stdexcept>

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

        // ⚠️ 必须自己判 exp 是否存在：jwt-cpp 内置的 exp 校验是「有则验，无则跳过」——
        //     if (!ctx.jwt.has_expires_at()) return;      // jwt-cpp/jwt.h 的 claims["exp"]
        // 也就是说**不带 exp 的 token 会永久有效，且不会报任何错**。
        //
        // 本工程签发端一定会写 exp（api_v1_User.cc 的 set_expires_at），
        // 所以这条强校验对合法 token 零影响。它挡的是两件事：
        //   1. 拿着密钥自签一个「永不过期」token（密钥在 config.json 里是明文）；
        //   2. 密钥轮换后，用旧密钥签发的无 exp token 仍然有效 —— 轮换等于没轮换。
        // 放在 verify 之后：签名错误的 token 先报签名错，错误信息更准确。
        if (!decoded.has_expires_at())
        {
            throw std::runtime_error("token has no exp claim");
        }

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
