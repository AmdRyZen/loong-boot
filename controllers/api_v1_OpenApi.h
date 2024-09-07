#pragma once

#include "threadPool/threadPool.h"
#include <drogon/HttpController.h>

extern ThreadPool pool;
using namespace drogon;
namespace api::v1 {
class OpenApi final : public drogon::HttpController<OpenApi>
{
  public:
    METHOD_LIST_BEGIN
    METHOD_ADD(OpenApi::mqtt, "/mqtt", Get);
    METHOD_ADD(OpenApi::coroutine, "/coroutine", Get);
    METHOD_ADD(OpenApi::algorithm, "/algorithm", Get);
    METHOD_ADD(OpenApi::aes, "/aes", Get);
    METHOD_ADD(OpenApi::simd, "/simd", Get);
    METHOD_ADD(OpenApi::boost, "/boost", Get);
    METHOD_ADD(OpenApi::curlPost, "/curl", Get);
    METHOD_ADD(OpenApi::getValue, "/getValue", Get);
    METHOD_ADD(OpenApi::fastJson, "/fastJson", Get);
    METHOD_ADD(OpenApi::threadPool, "/threadPool", Get);
    METHOD_ADD(OpenApi::fix, "/fix", Get);
    METHOD_ADD(OpenApi::random, "/random", Get);
    METHOD_ADD(OpenApi::taskflow, "/taskFlow", Get);
    // use METHOD_ADD to add your custom processing function here;
    // METHOD_ADD(OpenApi::get, "/{2}/{1}", Get); // path is /api/v1/OpenApi/{arg2}/{arg1}
    // METHOD_ADD(OpenApi::your_method_name, "/{1}/{2}/list", Get); // path is /api/v1/OpenApi/{arg1}/{arg2}/list
    // ADD_METHOD_TO(OpenApi::your_method_name, "/absolute/path/{1}/{2}/list", Get); // path is /absolute/path/{arg1}/{arg2}/list

    METHOD_LIST_END
    // your declaration of processing function maybe like this:
    // void get(const HttpRequestPtr& req, std::function<void (const HttpResponsePtr &)> &&callback, int p1, std::string p2);
    // void your_method_name(const HttpRequestPtr& req, std::function<void (const HttpResponsePtr &)> &&callback, double p1, int p2) const;

    static Task<> mqtt(HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback);

    static Task<> coroutine(HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback);

    static Task<> algorithm(HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback);

    static Task<> aes(HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback);

    static Task<> simd(HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback);

    static Task<> boost(HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback);

    static void curlPost(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

    static Task<> getValue(HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback);

    static Task<> fastJson(HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback);

    static Task<> threadPool(HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback);

    static Task<> fix(HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback);

    static Task<> random(HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback);

    static Task<> taskflow(HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback);
};
}  // namespace api::v1
