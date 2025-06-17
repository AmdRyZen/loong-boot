#pragma once
#include "filters/LoginFilter.h"
#include "service/TrieService.h"
#include <drogon/HttpController.h>

extern TrieService trieService;
using namespace drogon;
namespace api::v1 {
class User final : public drogon::HttpController<User>
{
  public:
    METHOD_LIST_BEGIN
    METHOD_ADD(User::buildSql, "/buildSql", Get);
    METHOD_ADD(User::login, "/login?userId={1}&passwd={2}", Post);
    METHOD_ADD(User::getInfo, "/{1}/getInfo?token={2}", Post, "drogon::LoginFilter");
    METHOD_ADD(User::getBanWord, "/getBanWord?word={1}", Get);
    METHOD_ADD(User::serdeJson, "/serdeJson", Get);
    METHOD_ADD(User::quit, "/quit", Get);
    //use METHOD_ADD to add your custom processing function here;
    //METHOD_ADD(User::get,"/{2}/{1}",Get);//path is /api/v1/User/{arg2}/{arg1}
    //METHOD_ADD(User::your_method_name,"/{1}/{2}/list",Get);//path is /api/v1/User/{arg1}/{arg2}/list
    //ADD_METHOD_TO(User::your_method_name,"/absolute/path/{1}/{2}/list",Get);//path is /absolute/path/{arg1}/{arg2}/list
    METHOD_LIST_END

    static Task<> buildSql(HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback);

    static void login(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      uint64_t&& userId,
                      const std::string& password);

    static Task<> getInfo(HttpRequestPtr req,
                          std::function<void(const HttpResponsePtr&)> callback,
                          std::string userId,
                          std::string token);

    static void getBanWord(const HttpRequestPtr& req,
                           std::function<void(const HttpResponsePtr&)>&& callback,
                           const std::string& word);

    static void serdeJson(const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback);
    // your declaration of processing function maybe like this:
    // void get(const HttpRequestPtr& req,std::function<void (const HttpResponsePtr &)> &&callback,int p1,std::string p2);
    // void your_method_name(const HttpRequestPtr& req,std::function<void (const HttpResponsePtr &)> &&callback,double p1,int p2) const;
    static void quit(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);
};
}  // namespace api::v1
