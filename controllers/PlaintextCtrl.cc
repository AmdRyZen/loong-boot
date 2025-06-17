#include "PlaintextCtrl.h"
void PlaintextCtrl::asyncHandleHttpRequest(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback)
{
    //write your application logic here
    //write your application logic here
    const auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k200OK);
    resp->setContentTypeCode(CT_TEXT_HTML);
    //resp->setBody("Hello World!");
    resp->setExpiredTime(0);
    callback(resp);
}