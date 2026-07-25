#pragma once

#include "SqlBuilderSupport.h"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sql
{
class SqlDelete : public detail::WhereBuilder<SqlDelete>
{
  public:
    static constexpr bool rejectEmptyNotIn = true;

    SqlDelete(drogon::orm::DbClientPtr client, std::string table)
        : client_(std::move(client)), table_(std::move(table))
    {
        if (!client_)
        {
            throw std::invalid_argument("无效的数据库客户端");
        }
        detail::requireIdentifier(table_, "表名");
    }

    SqlDelete& limit(std::size_t value)
    {
        if (value == 0)
        {
            throw std::invalid_argument("LIMIT 必须大于 0");
        }
        limit_ = value;
        return *this;
    }

    SqlDelete& allowFullTableDelete(bool enabled = true) noexcept
    {
        allowFullTableDelete_ = enabled;
        return *this;
    }

    [[nodiscard]] std::string toSql() const
    {
        validateExecutable();
        std::string statement;
        statement.reserve(24 + table_.size() + whereSqlCapacity());
        statement = "delete from ";
        statement += table_;
        appendWhere(statement);
        if (limit_ > 0)
        {
            statement += " limit ";
            statement += std::to_string(limit_);
        }
        return statement;
    }

    auto exec() const
    {
        return detail::executeWith(
            client_,
            toSql(),
            [this](drogon::orm::internal::SqlBinder& binder) {
                for (const auto& parameter : whereValues_)
                {
                    parameter.bind(binder);
                }
            });
    }

    template <typename T>
    auto softDelete(std::string field, T&& value) const
    {
        detail::requireIdentifier(field, "软删除字段");
        validateExecutable();

        std::string statement;
        statement.reserve(32 + table_.size() + field.size() +
                          whereSqlCapacity());
        statement = "update ";
        statement += table_;
        statement += " set ";
        statement += field;
        statement += " = ?";
        appendWhere(statement);
        if (limit_ > 0)
        {
            statement += " limit ";
            statement += std::to_string(limit_);
        }

        return detail::executeWith(
            client_,
            std::move(statement),
            [this,
             softDeleteValue = SqlValue(std::forward<T>(value))](
                drogon::orm::internal::SqlBinder& binder) {
                softDeleteValue.bind(binder);
                for (const auto& parameter : whereValues_)
                {
                    parameter.bind(binder);
                }
            });
    }

    template <std::ranges::input_range Range>
        requires(!std::convertible_to<Range, std::string_view>)
    static drogon::Task<std::uint64_t> batch(
        drogon::orm::DbClientPtr client,
        std::string table,
        std::string keyField,
        const Range& values,
        std::size_t batchSize = 500)
    {
        std::vector<SqlValue> copiedValues;
        if constexpr (std::ranges::sized_range<Range>)
        {
            copiedValues.reserve(std::ranges::size(values));
        }
        for (const auto& value : values)
        {
            copiedValues.emplace_back(value);
        }
        return batchValues(std::move(client),
                           std::move(table),
                           std::move(keyField),
                           std::move(copiedValues),
                           batchSize);
    }

  private:
    static drogon::Task<std::uint64_t> batchValues(
        drogon::orm::DbClientPtr client,
        std::string table,
        std::string keyField,
        std::vector<SqlValue> values,
        std::size_t batchSize)
    {
        if (!client)
        {
            throw std::invalid_argument("无效的数据库客户端");
        }
        if (batchSize == 0)
        {
            throw std::invalid_argument("批量删除分批大小必须大于 0");
        }
        detail::requireIdentifier(table, "表名");
        detail::requireIdentifier(keyField, "批量删除主键字段");

        std::uint64_t affectedRows = 0;
        for (std::size_t offset = 0; offset < values.size();)
        {
            const auto end = offset + std::min(batchSize,
                                               values.size() - offset);
            std::vector<SqlValue> chunk;
            chunk.reserve(end - offset);
            for (std::size_t i = offset; i < end; ++i)
            {
                chunk.push_back(std::move(values[i]));
            }

            auto deletion = SqlDelete(client, table);
            deletion.whereIn(keyField, chunk);
            const auto result = co_await deletion.exec();
            affectedRows += result.affectedRows();
            offset = end;
        }
        co_return affectedRows;
    }

    void validateExecutable() const
    {
        if (!hasWhere() && !allowFullTableDelete_)
        {
            throw std::invalid_argument(
                "拒绝执行没有 WHERE 条件的 DELETE；全表删除必须显式调用 "
                "allowFullTableDelete()");
        }
    }

    drogon::orm::DbClientPtr client_;
    std::string table_;
    std::size_t limit_{0};
    bool allowFullTableDelete_{false};
};
}  // namespace sql
