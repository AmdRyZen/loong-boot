#include "MetricsCtrl.h"
#include "utils/PrometheusMetrics.h"

void MetricsCtrl::asyncHandleHttpRequest(const drogon::HttpRequestPtr& req,
                                        std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    const auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString("text/plain; version=0.0.4; charset=utf-8");
    resp->setBody(Metrics::PrometheusRegistry::instance().exportPrometheusText());
    // ⚠️ drogon 的 setExpiredTime 语义与直觉相反（见 HttpResponse.h）：
    //    0  = **永久缓存**，负数 = 不缓存，默认 -1。
    // 写 0 会让每个 IO 线程把「它服务的第一个请求」的响应永久冻结
    // （缓存是 IOThreadStorage，每线程一份），此后该线程上的所有 /metrics
    // 抓取都返回那一份旧快照 —— 监控数据会静默失真。
    resp->setExpiredTime(-1);
    callback(resp);
}
