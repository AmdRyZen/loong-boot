#include "MetricsCtrl.h"
#include "utils/PrometheusMetrics.h"

void MetricsCtrl::asyncHandleHttpRequest(const drogon::HttpRequestPtr& req,
                                        std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString("text/plain; version=0.0.4; charset=utf-8");
    resp->setBody(Metrics::PrometheusRegistry::instance().exportPrometheusText());
    resp->setExpiredTime(0);
    callback(resp);
}
