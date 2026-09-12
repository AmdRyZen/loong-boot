#include "PlaintextCtrl.h"
void PlaintextCtrl::asyncHandleHttpRequest(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback)
{
    //write your application logic here
    //write your application logic here
    const auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k200OK);
    resp->setContentTypeCode(CT_TEXT_HTML);
    //resp->setBody("Hello World!");
    // ⚠️ drogon 语义：0 = 永久缓存，负数 = 不缓存（默认 -1）。写 0 会把响应
    // 按 IO 线程永久冻结。这里没有正文，但同样不该被缓存。
    resp->setExpiredTime(-1);
    callback(resp);
}