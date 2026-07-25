#pragma once

#include "SqlBuilderSupport.h"

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sql
{
enum class SqlOrder
{
    Asc,
    Desc
};

class SqlQuery : public detail::WhereBuilder<SqlQuery>
{
  public:
    static constexpr bool rejectEmptyNotIn = false;

    SqlQuery(drogon::orm::DbClientPtr client, std::string table)
        : client_(std::move(client)), table_(std::move(table))
    {
        if (!client_)
        {
            throw std::invalid_argument("无效的数据库客户端");
        }
        detail::requireIdentifier(table_, "表名");
    }

    SqlQuery& select(std::initializer_list<std::string> fields)
    {
        for (const auto& field : fields)
        {
            if (field != "*")
            {
                detail::requireIdentifier(field, "查询字段");
            }
            if (std::ranges::find(selectFields_, field) == selectFields_.end())
            {
                selectFields_.push_back(field);
            }
        }
        return *this;
    }

    SqlQuery& distinct(bool enabled = true) noexcept
    {
        distinct_ = enabled;
        return *this;
    }

    SqlQuery& groupBy(std::string field)
    {
        detail::requireIdentifier(field, "分组字段");
        if (std::ranges::find(groupBy_, field) == groupBy_.end())
        {
            groupBy_.push_back(std::move(field));
        }
        return *this;
    }

    template <typename... Args>
    SqlQuery& having(std::string condition, Args&&... args)
    {
        detail::validateWhere(condition, sizeof...(Args));
        havingClauses_.push_back(
            {detail::SqlConnector::And, std::move(condition)});
        (havingValues_.emplace_back(std::forward<Args>(args)), ...);
        return *this;
    }

    template <typename... Args>
    SqlQuery& orHaving(std::string condition, Args&&... args)
    {
        detail::validateWhere(condition, sizeof...(Args));
        havingClauses_.push_back(
            {detail::SqlConnector::Or, std::move(condition)});
        (havingValues_.emplace_back(std::forward<Args>(args)), ...);
        return *this;
    }

    SqlQuery& orderBy(std::string field, SqlOrder order = SqlOrder::Asc)
    {
        detail::requireIdentifier(field, "排序字段");
        const auto existing = std::ranges::find(
            orderBy_, field, &std::pair<std::string, SqlOrder>::first);
        if (existing == orderBy_.end())
        {
            orderBy_.emplace_back(std::move(field), order);
        }
        else
        {
            existing->second = order;
        }
        return *this;
    }

    SqlQuery& clearOrder() noexcept
    {
        orderBy_.clear();
        return *this;
    }

    SqlQuery& limit(std::size_t value)
    {
        if (value == 0)
        {
            throw std::invalid_argument("LIMIT 必须大于 0");
        }
        limit_ = value;
        return *this;
    }

    SqlQuery& offset(std::size_t value) noexcept
    {
        offset_ = value;
        return *this;
    }

    SqlQuery& page(std::size_t pageNumber, std::size_t pageSize)
    {
        if (pageNumber == 0 || pageSize == 0)
        {
            throw std::invalid_argument("页码和每页数量必须大于 0");
        }
        if (pageNumber - 1 >
            std::numeric_limits<std::size_t>::max() / pageSize)
        {
            throw std::overflow_error("分页偏移量溢出");
        }
        limit_ = pageSize;
        offset_ = (pageNumber - 1) * pageSize;
        return *this;
    }

    [[nodiscard]] std::string toSql() const
    {
        return buildSelectSql(true, true);
    }

    auto exec() const
    {
        return executeStatement(toSql());
    }

    template <typename T>
    drogon::Task<std::vector<T>> list() const &
    {
        validateRecordType<T>();
        const auto result = co_await exec();
        std::vector<T> records;
        records.reserve(result.size());
        for (const auto& row : result)
        {
            records.emplace_back(T::from(row));
        }
        co_return records;
    }

    template <typename T>
    drogon::Task<std::optional<T>> first() const &
    {
        validateRecordType<T>();
        auto query = *this;
        query.limit_ = 1;
        const auto result = co_await query.exec();
        if (result.empty())
        {
            co_return std::nullopt;
        }
        co_return T::from(result[0]);
    }

    template <typename T>
    drogon::Task<std::vector<T>> pluck(std::string field) const &
    {
        detail::requireIdentifier(field, "查询字段");
        auto query = *this;
        query.selectFields_ = {std::move(field)};
        query.distinct_ = false;

        const auto result = co_await query.exec();
        std::vector<T> values;
        values.reserve(result.size());
        for (const auto& row : result)
        {
            values.emplace_back(row[0].template as<T>());
        }
        co_return values;
    }

    template <typename T>
    drogon::Task<std::optional<T>> value(std::string field) const &
    {
        detail::requireIdentifier(field, "查询字段");
        auto query = *this;
        query.selectFields_ = {std::move(field)};
        query.distinct_ = false;
        query.limit_ = 1;

        const auto result = co_await query.exec();
        if (result.empty() || result[0][0].isNull())
        {
            co_return std::nullopt;
        }
        co_return result[0][0].template as<T>();
    }

    drogon::Task<bool> exists() const &
    {
        std::string statement;
        statement.reserve(32 + table_.size() + whereSqlCapacity());
        statement = "select 1 from ";
        statement += table_;
        appendWhere(statement);
        appendGroupAndHaving(statement);
        statement += " limit 1";

        const auto result = co_await executeStatement(std::move(statement));
        co_return !result.empty();
    }

    drogon::Task<std::uint64_t> count() const &
    {
        std::string statement;
        const bool singleDistinctField =
            distinct_ && selectFields_.size() == 1 &&
            selectFields_.front() != "*";
        const bool needsSubquery =
            !groupBy_.empty() || !havingClauses_.empty() ||
            (distinct_ && !singleDistinctField);

        if (!needsSubquery)
        {
            statement.reserve(56 + table_.size() + whereSqlCapacity());
            statement = "select count(";
            if (singleDistinctField)
            {
                statement += "distinct ";
                statement += selectFields_.front();
            }
            else
            {
                statement += '*';
            }
            statement += ") from ";
            statement += table_;
            appendWhere(statement);
        }
        else
        {
            const auto inner = buildSelectSql(false, false, "1");
            statement.reserve(inner.size() + 48);
            statement = "select count(*) from (";
            statement += inner;
            statement += ") as _loong_count";
        }

        const auto result = co_await executeStatement(std::move(statement));
        if (result.empty() || result[0][0].isNull())
        {
            co_return 0;
        }
        co_return result[0][0].as<std::uint64_t>();
    }

  private:
    template <typename T>
    static consteval void validateRecordType()
    {
        static_assert(requires(const drogon::orm::Row& row) {
            { T::from(row) } -> std::convertible_to<T>;
        }, "查询结果类型必须提供 static T from(const drogon::orm::Row&)");
    }

    drogon::orm::internal::SqlAwaiter executeStatement(
        std::string statement) const
    {
        return detail::executeWith(
            client_,
            std::move(statement),
            [this](drogon::orm::internal::SqlBinder& binder) {
                for (const auto& parameter : whereValues_)
                {
                    parameter.bind(binder);
                }
                for (const auto& parameter : havingValues_)
                {
                    parameter.bind(binder);
                }
            });
    }

    void appendGroupAndHaving(std::string& statement) const
    {
        if (!groupBy_.empty())
        {
            statement += " group by ";
            for (std::size_t i = 0; i < groupBy_.size(); ++i)
            {
                if (i > 0)
                {
                    statement += ", ";
                }
                statement += groupBy_[i];
            }
        }
        detail::appendConditions(statement,
                                 " having ",
                                 havingClauses_);
    }

    [[nodiscard]] std::string buildSelectSql(
        bool includeOrder,
        bool includePagination,
        std::string_view selectionOverride = {}) const
    {
        std::size_t capacity = 40 + table_.size() + whereSqlCapacity();
        for (const auto& field : selectFields_)
            capacity += field.size() + 2;
        for (const auto& field : groupBy_)
            capacity += field.size() + 2;
        for (const auto& clause : havingClauses_)
            capacity += clause.condition.size() + 8;
        for (const auto& [field, order] : orderBy_)
        {
            static_cast<void>(order);
            capacity += field.size() + 7;
        }

        std::string statement;
        statement.reserve(capacity);
        statement = "select ";
        if (distinct_ && selectionOverride.empty())
        {
            statement += "distinct ";
        }

        if (!selectionOverride.empty())
        {
            statement += selectionOverride;
        }
        else if (selectFields_.empty())
        {
            statement += '*';
        }
        else
        {
            for (std::size_t i = 0; i < selectFields_.size(); ++i)
            {
                if (i > 0)
                    statement += ", ";
                statement += selectFields_[i];
            }
        }

        statement += " from ";
        statement += table_;
        appendWhere(statement);
        appendGroupAndHaving(statement);

        if (includeOrder && !orderBy_.empty())
        {
            statement += " order by ";
            for (std::size_t i = 0; i < orderBy_.size(); ++i)
            {
                if (i > 0)
                    statement += ", ";
                statement += orderBy_[i].first;
                statement += orderBy_[i].second == SqlOrder::Desc
                                 ? " desc"
                                 : " asc";
            }
        }

        if (includePagination && limit_ > 0)
        {
            statement += " limit ";
            statement += std::to_string(limit_);
        }
        if (includePagination && offset_ > 0)
        {
            if (limit_ == 0)
            {
                throw std::invalid_argument("OFFSET 必须和 LIMIT 一起使用");
            }
            statement += " offset ";
            statement += std::to_string(offset_);
        }
        return statement;
    }

    drogon::orm::DbClientPtr client_;
    std::string table_;
    std::vector<std::string> selectFields_;
    std::vector<std::string> groupBy_;
    std::vector<detail::WhereClause> havingClauses_;
    std::vector<SqlValue> havingValues_;
    std::vector<std::pair<std::string, SqlOrder>> orderBy_;
    std::size_t limit_{0};
    std::size_t offset_{0};
    bool distinct_{false};
};
}  // namespace sql
