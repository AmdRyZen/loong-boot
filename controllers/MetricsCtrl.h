#pragma once
#include <drogon/HttpSimpleController.h>

class MetricsCtrl final : public drogon::HttpSimpleController<MetricsCtrl>
{
public:
    void asyncHandleHttpRequest(const drogon::HttpRequestPtr& req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& callback) override;

    PATH_LIST_BEGIN
    PATH_ADD("/metrics", drogon::Get);
    PATH_LIST_END
};
