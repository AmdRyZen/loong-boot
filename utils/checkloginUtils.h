//
// Created by 神圣·凯莎 on 2022/6/24.
//

#ifndef DROGON_HTTP_CHECKLOGINUTILS_H
#define DROGON_HTTP_CHECKLOGINUTILS_H

#include <drogon/HttpAppFramework.h>
#include <jwt-cpp/jwt.h>
#include <optional>

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
        LOG_ERROR << "checklogin err = " << e.what() << " login";
        return std::nullopt;
    }
}
#endif  // DROGON_HTTP_CHECKLOGINUTILS_H
