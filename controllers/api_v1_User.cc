#include "api_v1_User.h"
#include "jwt-cpp/jwt.h"
#include "utils/redisUtils.h"
#include "filters/SqlFilter.h"
#include <filesystem>
#include <fstream>
#include <drogon/drogon.h>
#include "utils/sql.h"
#include "utils/tbbUtils.h"
#include "service/SbcConvertService.h"
#include <algorithm>
#include <execution>
#include "base/base.h"
#include "base/vo/data_vo.h"
#include "base/dto/open_dto.h"
#include "utils/SqlUpdate.h"
#include "utils/SqlQuery.h"
#include <tbb/concurrent_vector.h>

using namespace api::v1;
using namespace drogon::orm;
using namespace drogon;
using namespace sql;

Task<> User::buildSql(const HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback)
{
    auto method  = req->getMethod();
    std::string msg = "success";
    try
    {
         // Insert
        InsertModel i;
        i.insert("score", 100)
                ("name", std::string("six"))
                ("age", 20)
                ("address", "beijing")
                ("create_time", nullptr)
            .into("user");
        assert(i.str() ==
                "insert into user(score, name, age, address, create_time) values(100, 'six', 20, 'beijing', null)");

        // Insert with named parameters
        InsertModel iP;
        Param score(":score");
        Param name(":name");
        Param age(":age");
        Param address(":address");
        Param create_time(":create_time");
        iP.insert("score", score)
                ("name", name)
                ("age", age)
                ("address", address)
                ("create_time", create_time)
            .into("user");
        assert(iP.str() ==
                "insert into user(score, name, age, address, create_time) values(:score, :name, :age, :address, :create_time)");

        // Select
        SelectModel s;
        s.select("id as user_id", "age", "name", "address")
            .distinct()
            .from("user")
            .join("score")
            .on(column("user.id") == column("score.id") and column("score.id") > 60)
            .where(column("score") > 60 and (column("age") >= 20 or column("address").is_not_null()))
            // .where(column("score") > 60 && (column("age") >= 20 || column("address").is_not_null()))
            .group_by("age")
            .having(column("age") > 10)
            .order_by("age desc")
            .limit(10)
            .offset(1);
        assert(s.str() ==
                "select distinct id as user_id, age, name, address from user join score on (user.id = score.id) and (score.id > 60) where (score > 60) and ((age >= 20) or (address is not null)) group by age having age > 10 order by age desc limit 10 offset 1");

        // Update
        std::vector<int> a = {1, 2, 3};
        UpdateModel u;
        u.update("user")
            .set("name", "ddc")
                ("age", 18)
                ("score", nullptr)
                ("address", "beijing")
            .where(column("id").in(a));
        assert(u.str() ==
                "update user set name = 'ddc', age = 18, score = null, address = 'beijing' where id in (1, 2, 3)");

        // Update with positional parameters
        UpdateModel uP;
        Param mark("?");
        uP.update("user")
            .set("name", mark)
                ("age", mark)
                ("score", mark)
                ("address", mark)
            .where(column("id").in(a));
        assert(uP.str() ==
                "update user set name = ?, age = ?, score = ?, address = ? where id in (1, 2, 3)");

        // Delete
        DeleteModel d;
        d._delete()
            .from("user")
            .where(column("id") == 1);
        assert(d.str() ==
                "delete from user where id = 1");


        // 基本 SQL 查询语句
        std::string baseCountSql = "SELECT count(1) FROM xxl_job_info WHERE 1=1";

        SelectModel select;
        select.select("*")
            .distinct()
            .from("xxl_job_info")
            .order_by("id desc")
            .limit(10);
        SelectModel selectCount;
        selectCount.select("count(1) as count")
            .from("xxl_job_info");
        if constexpr (true) [[likely]]
        {
            select.where(column("id") < 3 and column("author") != "xxx");
            select.where(column("id") > 0);
            selectCount.where(column("id") < 3 and column("author") != "xxx");
        }

        std::cout << "select: " << select.str() << std::endl;
        std::cout << "selectCount: " << selectCount.str() << std::endl;

        auto clientPtr = drogon::app().getFastDbClient();

        auto xxx = co_await clientPtr->execSqlCoro(select.str());
        auto xxxCount = co_await clientPtr->execSqlCoro(selectCount.str());
        std::cout << "xxx: " << xxx.size() << std::endl;
        std::cout << "xxxCount: " <<  xxxCount[0][0].as<std::size_t>() << std::endl;

    }
    catch (...)
    {
       msg = "error";
    }
    co_return callback(Base<HttpMethod>::createHttpSuccessResponse(StatusOK, msg, method));
}

Task<> User::dynamicUpdateJobAuthor(
    const HttpRequestPtr req,
    std::function<void(const HttpResponsePtr&)> callback)
{
    try
    {
        const auto body = req->getJsonObject();
        if (!body || !body->isMember("updates") ||
            !(*body)["updates"].isArray() || (*body)["updates"].empty())
        {
            co_return callback(Base<std::string>::createHttpErrorResponse(
                StatusError,
                "请求格式应为 {\"updates\":[{\"id\":1,\"author\":\"aa\"}]}",
                ""));
        }

        const auto clientPtr = app().getFastDbClient();
        auto transPtr = co_await clientPtr->newTransactionCoro();
        DynamicUpdateResponseVo response;
        try
        {
            for (const auto& update : (*body)["updates"])
            {
                if (!update.isObject() || !update["id"].isInt() ||
                    !update["author"].isString())
                {
                    throw std::invalid_argument(
                        "每个 update 都必须包含整数 id 和字符串 author");
                }

                const auto result = co_await SqlUpdate(transPtr, "xxl_job_info")
                    .set({{"author", update["author"]}})
                    .where("id = ?", update["id"].asInt())
                    .limit(1)
                    .exec();
                response.affected_rows.push_back(result.affectedRows());
            }

            // 查询只执行一次
            const auto queryResult = co_await SqlQuery(transPtr, "xxl_job_info")
                .select({"id", "author"})
                .where("id != ?", (*body)["updates"][0]["id"].asInt())
                .orderBy("id", SqlOrder::Desc)
                .limit(10)
                .exec();

            for (const auto& row : queryResult)
            {
                DynamicUpdateRecordVo record;
                record.id = row["id"].as<std::int64_t>();
                if (!row["author"].isNull())
                {
                    record.author = row["author"].as<std::string>();
                }
                response.records.push_back(std::move(record));
            }
        }
        catch (...)
        {
            transPtr->rollback();
            throw;
        }

        response.update_count = (*body)["updates"].size();
        co_return callback(Base<DynamicUpdateResponseVo>::createHttpSuccessResponse(
            StatusOK, Success, std::move(response)));
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "dynamicUpdateJobAuthor failed: " << e.what();
        co_return callback(Base<std::string>::createHttpErrorResponse(
            StatusError, e.what(), ""));
    }
}

//add definition of your processing function here
void User::login(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 uint64_t&& userId,
                 const std::string& password)
{
    try
    {
        if (req->getBody().empty())
        {
            return callback(Base<std::string>::createHttpSuccessResponse(StatusOK, "json is empty", ""));
        }

        MemberInfoDto memberInfoDto{};
        if (glz::read_json(memberInfoDto, req->getBody()))
        {
            return callback(Base<std::string>::createHttpSuccessResponse(StatusOK, Success, ""));
        }
        // ...

        const auto token = jwt::create()
                         .set_issuer("auth0")
                         .set_type("JWS")
                         .set_payload_claim("user_id", jwt::claim(std::to_string(userId)))
                         .set_payload_claim("user_name", jwt::claim(memberInfoDto.user_name))
                         .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{drogon::app().getCustomConfig()["jwt-sessionTime"].asInt()})
                         .sign(jwt::algorithm::hs256{drogon::app().getCustomConfig()["jwt-secret"].asString()});

        MemberInfoVo memberInfoVo{};

        memberInfoVo.name = memberInfoDto.user_name;
        memberInfoVo.token = token;
        memberInfoVo.user_id = userId;

        return callback(Base<MemberInfoVo>::createHttpSuccessResponse(StatusOK, Success, memberInfoVo));
    }
    catch (...)
    {
        return callback(Base<std::string>::createHttpSuccessResponse(StatusOK, "loginError", ""));
    }
}

Task<> User::getInfo(const HttpRequestPtr req,
                     std::function<void(const HttpResponsePtr&)> callback,
                     std::string userId,
                     const std::string token)
{
    LOG_INFO << "User " << userId << " get his information";

    auto clientPtr = app().getFastDbClient();

    UserDataListVo user_data_list_vo{};
    try
    {
        // 基本 SQL 查询语句
        const std::string baseSql = "SELECT * FROM xxl_job_info";
        const std::string baseCountSql = "SELECT count(1) FROM xxl_job_info";

        try {

            // 111111
            std::vector<Condition> conditionsParams = {
                {"id", ">", 10, LogicOp::And, true},
                {"id", "<", 100, LogicOp::And, false, true},
                {"author", "=", "admin", LogicOp::Or}
            };

            auto sql = SqlFilter::BuildConditionsSQLWithParams(conditionsParams);
            std::cout << "1111111 构建 WHERE 子句: " << sql << std::endl;
            // sql: " WHERE (id > ? AND id < ?) OR author = ?"
            // params: ["10", "100", "admin"]



            // 构造条件表达式树： 2222222
            // (id > 10 AND id < 100) OR (author = 'admin' AND status = 1)
            SqlFilter::ConditionExpr rootExpr {
                .condition = Condition{},
                .children = {
                    SqlFilter::ConditionExpr {
                        .condition = Condition{},
                        .children = {
                            SqlFilter::ConditionExpr{ .condition = Condition{"id", ">", 10}, .children = {}, .logic = LogicOp::And, .grouped = false },
                            SqlFilter::ConditionExpr{ .condition = Condition{"id", "<", 100}, .children = {}, .logic = LogicOp::And, .grouped = false }
                        },
                        .logic = LogicOp::And,
                        .grouped = true
                    },
                    SqlFilter::ConditionExpr {
                        .condition = Condition{},
                        .children = {
                            SqlFilter::ConditionExpr{ .condition = Condition{"author", "=", std::string("admin")}, .children = {}, .logic = LogicOp::And, .grouped = false },
                            SqlFilter::ConditionExpr{ .condition = Condition{"status", "=", 1}, .children = {}, .logic = LogicOp::And, .grouped = false }
                        },
                        .logic = LogicOp::And,
                        .grouped = true
                    }
                },
                .logic = LogicOp::Or,
                .grouped = false
            };

            // 调用构造 SQL 字符串函数（改造后只返回字符串）
            std::string sql1 = SqlFilter::BuildSQLFromExprTreeWithValues(rootExpr);
            std::cout << "2222222 生成的 SQL1 WHERE: " << sql1 << std::endl;



            // 33333333
            std::vector<Condition> conditions = {
                {"id", "<", 3},
                {"author", "!=", std::string("xxx")}
            };

            if (MemberInfoDto memberInfoDto{}; !req->getBody().empty() && !glz::read_json(memberInfoDto, req->getBody()))
            {
                conditions.push_back({"author", "!=", memberInfoDto.user_name});
            }
            // 构建 WHERE 子句
            const std::string whereClause = SqlFilter::BuildConditionsSQL(conditions);
            // 拼接完整 SQL
            const std::string dynamicSql = baseSql + whereClause;
            const std::string dynamicCountSql = baseCountSql + whereClause;
            // 输出调试信息
            std::cout << "构建 WHERE 子句: " << whereClause << std::endl;
            std::cout << "动态 SQL 查询语句: " << dynamicSql << std::endl;
            std::cout << "动态 Count 查询语句: " << dynamicCountSql << std::endl;

            auto result = co_await clientPtr->execSqlCoro(dynamicSql);
            auto count = co_await clientPtr->execSqlCoro(dynamicCountSql);

            user_data_list_vo.list = smart_parallel_transform<size_t>(result.size(), [&](const size_t i) {
                auto user = UserDataItem::from(result[i]);
                return user;
            });

            user_data_list_vo.user_map = smart_parallel_transform_to_unordered_map<size_t, int64_t, UserDataItem>(
                 result.size(),
                 [&](const size_t i) {
                     auto user = UserDataItem::from(result[i]);
                     return std::make_pair(user.id, std::move(user));
                 }
            );

            /*auto list = SqlFilter::smart_parallel_transform_to_container<
               size_t,
               std::vector<UserDataItem>
           >(
               result.size(),
               [&](const size_t i) {
                   UserDataItem data_item;
                   data_item.id = result[i]["id"].template as<std::int64_t>();
                   data_item.author = result[i]["author"].template as<std::string>();
                   data_item.job_desc = result[i]["job_desc"].template as<std::string>();
                   return data_item;
               }
           );*/

            user_data_list_vo.num_users = count[0][0].as<std::int32_t>();

        } catch (const std::exception& e) {
            std::cerr << "构建 SQL 失败，原因: " << e.what() << std::endl;
        }

        *clientPtr  << "select * from xxl_job_info where author != ? and id = ?"
            << "xxx" << 1
            >> [](const Result &result)
            {
                std::cout << result.size() << " rows selected!" << std::endl;
                int i = 0;
                for (const auto& row : result)
                {
                    std::cout << i++ << ": author is " << row["author"].as<std::string>() << std::endl;
                }
            }
        >> [](const DrogonDbException &e)
        {
            std::cerr << "error1:" << e.base().what() << std::endl;
        };


        *clientPtr << "select "
                     "    a.id, "
                     "    a.author "
                     "from "
                     "    xxl_job_info a "
                     "inner join xxl_job_info u on "
                     "    a.id = u.id "
                     "where u.author = ?"
                  << "xxx"
                >> [](const Result &result)
        {
            std::cout << result.size() << " rows selected!" << std::endl;
            int i = 0;

            //#pragma omp parallel for
            for (const auto& row : result)
            {
                std::cout << i++ << ": author is " << row["author"].as<std::string>() << std::endl;
            }
        }
            >> [](const DrogonDbException &e)
        {
            std::cerr << "error2:" << e.base().what() << std::endl;
        };

        // 创建SqlBinder对象
        std::string sql = "SELECT * FROM xxl_job_info WHERE id = ? and author = ?";
        auto nn = *clientPtr << sql;
        std::vector<std::string> values = {"1", "aa"};
        for (const auto &value : values)
        {
            nn << value;
        }
        nn >> [](const Result &r) {
            std::cout << "r.affectedRows() : " << r.affectedRows() << std::endl;
        };


        std::string sql1 = "SELECT * FROM xxl_job_info WHERE id = 1 and author = 'aa'";
        auto zz = clientPtr->execSqlCoro(sql1);

        auto r = co_await zz;
        std::cout << "Affected rows: " << r.affectedRows() << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "error3:" << e.what() << std::endl;
    }

    MemberInfoVo memberInfo{};
    if (glz::read_json(memberInfo, co_await redisUtils::getCoroRedisValue("aa")))
    {
        LOG_ERROR << "Failed to parse read_json";
    }
    user_data_list_vo.redis_value = std::move(memberInfo);

    co_return callback(Base<UserDataListVo>::createHttpSuccessResponse(StatusOK, Success, user_data_list_vo));
}

void User::getBanWord(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      const std::string& word)
{
    std::vector<std::string> words = {
        // 字母
        "FUCK",      // 全大写
        "FuCk",      // 混合
        "F&uc&k",    // 特殊符号
        "F&uc&&&k",  // 连续特殊符号
        "ＦＵｃｋ",  // 全角大小写混合
        "F。uc——k",  // 全角特殊符号
        "fＵｃk",    // 全角半角混合
        "fＵ😊ｃk",  // Emotion表情，测试

        // 简体中文
        "微信",
        "微——信",              // 全角符号
        "微【】、。？《》信",  // 全角重复词
        "微。信",
        "VX",
        "vx",                                   // 小写
        "V&X",                                  // 特殊字符
        "微!., #$%&*()|?/@\"';[]{}+~-_=^<>信",  // 30个特殊字符 递归
        "扣扣",
        "扣_扣",
        "QQ",
        "Qq",
    };

    std::ranges::for_each(words.begin(), words.end(), [&](const auto& item) {
        auto const t1 = std::chrono::steady_clock::now();

        std::wstring const result = TrieService::replaceSensitive(SbcConvertService::s2ws(item));

        auto const t2 = std::chrono::steady_clock::now();
        double const dr_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        std::cout << "[cost: " << dr_ms << " ms]" << item << " => " << SbcConvertService::ws2s(result) << std::endl;
    });

    callback(Base<std::string>::createHttpSuccessResponse(
        StatusOK,
        Success,
        SbcConvertService::ws2s(TrieService::replaceSensitive(SbcConvertService::s2ws(word))))
    );
}

void User::serdeJson(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback)
{
    std::string word_path;
    word_path.append(std::filesystem::current_path()).append("/public/world-cities.geojson");
    std::ifstream ifs(word_path, std::ios_base::in);
    std::string str;
    getline(ifs, str);
    //std::cout << " serdeJson = " << str << std::endl;

    auto t1 = std::chrono::steady_clock::now();

    bool res;
    JSONCPP_STRING errs;
    Json::Value root, list;
    Json::CharReaderBuilder readerBuilder;

    std::unique_ptr<Json::CharReader> const jsonReader(readerBuilder.newCharReader());
    res = jsonReader->parse(str.c_str(), str.c_str() + str.length(), &root, &errs);
    if (!res || !errs.empty())
    {
        std::cout << "parseJson err. " << errs << std::endl;
        return;
    }

    auto t2 = std::chrono::steady_clock::now();
    double dr_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::cout << "[cost: " << dr_ms << " ms]" << std::endl;
    std::cout << "type: " << root["type"].asString() << std::endl;

    callback(Base<std::string>::createHttpSuccessResponse(StatusOK, Success, ""));
}

void User::quit(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback)
{
    app().quit();
    const auto data = HttpResponse::newHttpResponse();
    data->setStatusCode(k204NoContent);
    callback(data);
}
