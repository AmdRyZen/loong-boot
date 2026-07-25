#define DROGON_TEST_MAIN
#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include "../utils/SqlBuilderSupport.h"
#include "../utils/SqlQuery.h"
#include "../utils/SqlUpdate.h"

namespace
{
struct TestWhereBuilder : sql::detail::WhereBuilder<TestWhereBuilder>
{
    static constexpr bool rejectEmptyNotIn = false;
};

struct StrictWhereBuilder : sql::detail::WhereBuilder<StrictWhereBuilder>
{
    static constexpr bool rejectEmptyNotIn = true;
};

[[maybe_unused]] void compileQueryApis(sql::SqlQuery& query)
{
    const std::vector<int> ids{1, 2};
    const std::optional<int> status{1};
    query.select({"id", "author"})
        .where<"id > ?">(0)
        .whereIn("id", ids)
        .whereBetween("id", 1, 10)
        .whereIfPresent("status = ?", status)
        .orWhereEq("author", "root")
        .groupBy("author")
        .having("count(*) > ?", 1)
        .orderBy("id", sql::SqlOrder::Desc)
        .page(1, 20);
    static_cast<void>(query.pluck<std::int64_t>("id"));
    static_cast<void>(query.value<std::string>("author"));
    static_cast<void>(query.count());
    static_cast<void>(query.exists());
}

[[maybe_unused]] void compileUpdateApis(sql::SqlUpdate& update)
{
    const std::vector<int> ids{1, 2};
    update.set("author", "root")
        .setIf(true, "status", 1)
        .increment("retry_count")
        .decrement("quota", 2)
        .whereIn("id", ids)
        .limit(2);
}
}

DROGON_TEST(BasicTest)
{
    CHECK(sql::detail::isIdentifier("xxl_job_info"));
    CHECK(sql::detail::isIdentifier("job.id"));
    CHECK(!sql::detail::isIdentifier("1job"));
    CHECK(!sql::detail::isIdentifier("job..id"));

    CHECK(sql::detail::placeholderCount("id = ? and author = ?") == 2);
    CHECK(sql::detail::placeholderCount(
              "JSON_EXTRACT(data, '$.question?') = ?") == 1);
    CHECK(sql::detail::placeholderCount(
              "name = '?' and id = ? /* ignored ? */") == 1);

    CHECK_NOTHROW(sql::detail::validateWhere("id = ?", 1));
    CHECK_THROWS_AS(sql::detail::validateWhere("id = ?", 0),
                    std::invalid_argument);

    TestWhereBuilder compileTimeWhere;
    compileTimeWhere.where<"id = ? and author != ?">(1, "root");
    CHECK(compileTimeWhere.hasWhere());

    StrictWhereBuilder strictWhere;
    const std::vector<int> emptyIds;
    CHECK_THROWS_AS(strictWhere.whereNotIn("id", emptyIds),
                    std::invalid_argument);

    CHECK(sql::SqlValue(1) == sql::SqlValue(std::int64_t{1}));
    CHECK(sql::SqlValue("aa") != sql::SqlValue("bb"));

    auto batch = sql::SqlUpdate::prepareBatch(
        "xxl_job_info",
        "id",
        {
            {1, {{"author", "aa"}}},
            {2, {{"author", "bb"}}},
            {1, {{"author", "cc"}}}
        });
    CHECK(batch.rowCount == 2);
    CHECK(batch.parameters.size() == 6);
    CHECK(batch.statement ==
          "update xxl_job_info set author = case id when ? then ? when ? "
          "then ? else author end where id in (?, ?)");
}

int main(const int argc, char** argv)
{
    using namespace drogon;

    std::promise<void> p1;
    std::future<void> f1 = p1.get_future();

    // Start the main loop on another thread
    std::thread thr([&]() {
        // Queues the promise to be fulfilled after starting the loop
        app().getLoop()->queueInLoop([&p1]() {
            p1.set_value();
        });
        app().run();
    });

    // The future is only satisfied after the event loop started
    f1.get();
    const int status = test::run(argc, argv);

    // Ask the event loop to shutdown and wait
    app().getLoop()->queueInLoop([]() {
        app().quit();
    });
    thr.join();
    return status;
}
