//
// Created by 神圣·凯莎 on 2022/6/25.
//

#ifndef DROGON_HTTP_LOGINFILTER_H
#define DROGON_HTTP_LOGINFILTER_H

#pragma once
#include "utils/checkloginUtils.h"
#include <drogon/HttpFilter.h>
#include "base/base.h"
#include "base/vo/data_vo.h"

using namespace drogon;

namespace drogon {
class DROGON_EXPORT LoginFilter final : public HttpFilter<LoginFilter>
{
  public:
    LoginFilter() = default;

    void doFilter(const HttpRequestPtr& req,
                  FilterCallback&& fcb,
                  FilterChainCallback&& fccb) override;
};
}  // namespace drogon

[[gnu::always_inline]] inline void LoginFilter::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb)
{
    //Edit your logic here
    if (checkloginUtils::checklogin(req).has_value())
    {
        //Passed
        fccb();
        return;
    }

    //Check failed
    fcb(Base<std::string>::createHttpUnauthorizedResponse(k401Unauthorized, NoLogin, ""));
}

#endif  // DROGON_HTTP_LOGINFILTER_H
