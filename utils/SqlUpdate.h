#pragma once

#include "SqlBuilderSupport.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sql
{
class SqlUpdate : public detail::WhereBuilder<SqlUpdate>
{
  public:
    static constexpr bool rejectEmptyNotIn = true;

    using FieldValue = std::pair<std::string, SqlValue>;

    struct BatchRow
    {
        SqlValue key;
        std::vector<FieldValue> values;
    };

    struct PreparedBatch
    {
        std::string statement;
        std::vector<SqlValue> parameters;
        std::size_t rowCount{};
    };

    SqlUpdate(drogon::orm::DbClientPtr client, std::string table)
        : client_(std::move(client)), table_(std::move(table))
    {
        if (!client_)
        {
            throw std::invalid_argument("无效的数据库客户端");
        }
        detail::requireIdentifier(table_, "表名");
    }

    SqlUpdate& set(std::initializer_list<FieldValue> values)
    {
        for (const auto& [field, value] : values)
        {
            set(field, value);
        }
        return *this;
    }

    template <typename T>
    SqlUpdate& set(std::string field, T&& value)
    {
        return setAssignment(std::move(field),
                             "?",
                             {SqlValue(std::forward<T>(value))});
    }

    template <typename T>
    SqlUpdate& setIf(bool enabled, std::string field, T&& value)
    {
        if (enabled)
        {
            set(std::move(field), std::forward<T>(value));
        }
        return *this;
    }

    template <typename T = std::int64_t>
    SqlUpdate& increment(std::string field, T amount = 1)
    {
        detail::requireIdentifier(field, "更新字段");
        const std::string expression = field + " + ?";
        return setAssignment(std::move(field),
                             expression,
                             {SqlValue(amount)});
    }

    template <typename T = std::int64_t>
    SqlUpdate& decrement(std::string field, T amount = 1)
    {
        detail::requireIdentifier(field, "更新字段");
        const std::string expression = field + " - ?";
        return setAssignment(std::move(field),
                             expression,
                             {SqlValue(amount)});
    }

    SqlUpdate& limit(std::size_t value)
    {
        if (value == 0)
        {
            throw std::invalid_argument("LIMIT 必须大于 0");
        }
        limit_ = value;
        return *this;
    }

    [[nodiscard]] std::string toSql() const
    {
        validateExecutable();

        std::size_t capacity = 32 + table_.size() + whereSqlCapacity();
        for (const auto& assignment : assignments_)
        {
            capacity += assignment.field.size() +
                        assignment.expression.size() + 5;
        }

        std::string statement;
        statement.reserve(capacity);
        statement = "update ";
        statement += table_;
        statement += " set ";

        for (std::size_t i = 0; i < assignments_.size(); ++i)
        {
            if (i > 0)
                statement += ", ";
            statement += assignments_[i].field;
            statement += " = ";
            statement += assignments_[i].expression;
        }

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
                for (const auto& assignment : assignments_)
                {
                    for (const auto& parameter : assignment.values)
                    {
                        parameter.bind(binder);
                    }
                }
                for (const auto& parameter : whereValues_)
                {
                    parameter.bind(binder);
                }
            });
    }

    static PreparedBatch prepareBatch(std::string table,
                                      std::string keyField,
                                      std::vector<BatchRow> rows)
    {
        detail::requireIdentifier(table, "表名");
        detail::requireIdentifier(keyField, "批量更新主键字段");

        normalizeBatchRows(rows, keyField);
        if (rows.empty())
        {
            throw std::invalid_argument("批量更新至少需要一条有效数据");
        }

        std::vector<std::string> fields;
        for (const auto& row : rows)
        {
            for (const auto& [field, value] : row.values)
            {
                static_cast<void>(value);
                if (std::ranges::find(fields, field) == fields.end())
                {
                    fields.push_back(field);
                }
            }
        }
        if (fields.empty())
        {
            throw std::invalid_argument("批量更新至少需要一个更新字段");
        }

        std::size_t caseCount = 0;
        for (const auto& field : fields)
        {
            for (const auto& row : rows)
            {
                if (findField(row.values, field) != row.values.end())
                {
                    ++caseCount;
                }
            }
        }

        std::vector<SqlValue> parameters;
        parameters.reserve(caseCount * 2 + rows.size());

        std::string statement;
        statement.reserve(64 + table.size() + keyField.size() +
                          fields.size() * 48 + rows.size() * 20);
        statement = "update ";
        statement += table;
        statement += " set ";

        for (std::size_t fieldIndex = 0;
             fieldIndex < fields.size();
             ++fieldIndex)
        {
            if (fieldIndex > 0)
                statement += ", ";

            const auto& field = fields[fieldIndex];
            statement += field;
            statement += " = case ";
            statement += keyField;

            for (const auto& row : rows)
            {
                const auto value = findField(row.values, field);
                if (value == row.values.end())
                    continue;

                statement += " when ? then ?";
                parameters.push_back(row.key);
                parameters.push_back(value->second);
            }

            statement += " else ";
            statement += field;
            statement += " end";
        }

        statement += " where ";
        statement += keyField;
        statement += " in (";
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            if (i > 0)
                statement += ", ";
            statement += '?';
            parameters.push_back(rows[i].key);
        }
        statement += ')';

        return {std::move(statement),
                std::move(parameters),
                rows.size()};
    }

    static auto batchOnce(drogon::orm::DbClientPtr client,
                          std::string table,
                          std::string keyField,
                          std::vector<BatchRow> rows)
    {
        if (!client)
        {
            throw std::invalid_argument("无效的数据库客户端");
        }
        auto prepared = prepareBatch(std::move(table),
                                     std::move(keyField),
                                     std::move(rows));
        return detail::execute(client,
                               std::move(prepared.statement),
                               prepared.parameters);
    }

    static drogon::Task<std::uint64_t> batch(
        drogon::orm::DbClientPtr client,
        std::string table,
        std::string keyField,
        std::vector<BatchRow> rows,
        std::size_t batchSize = 500)
    {
        if (!client)
        {
            throw std::invalid_argument("无效的数据库客户端");
        }
        if (batchSize == 0)
        {
            throw std::invalid_argument("批量更新分批大小必须大于 0");
        }

        detail::requireIdentifier(table, "表名");
        detail::requireIdentifier(keyField, "批量更新主键字段");
        normalizeBatchRows(rows, keyField);
        if (rows.empty())
        {
            throw std::invalid_argument("批量更新至少需要一条有效数据");
        }

        std::uint64_t affectedRows = 0;
        for (std::size_t offset = 0; offset < rows.size();)
        {
            const auto end = offset + std::min(batchSize,
                                               rows.size() - offset);
            std::vector<BatchRow> chunk;
            chunk.reserve(end - offset);
            for (std::size_t i = offset; i < end; ++i)
            {
                chunk.push_back(std::move(rows[i]));
            }

            const auto result = co_await batchOnce(client,
                                                   table,
                                                   keyField,
                                                   std::move(chunk));
            affectedRows += result.affectedRows();
            offset = end;
        }
        co_return affectedRows;
    }

  private:
    struct Assignment
    {
        std::string field;
        std::string expression;
        std::vector<SqlValue> values;
    };

    using FieldIterator = std::vector<FieldValue>::iterator;
    using ConstFieldIterator = std::vector<FieldValue>::const_iterator;

    static FieldIterator findField(std::vector<FieldValue>& values,
                                   const std::string& field)
    {
        return std::ranges::find(values, field, &FieldValue::first);
    }

    static ConstFieldIterator findField(const std::vector<FieldValue>& values,
                                        const std::string& field)
    {
        return std::ranges::find(values, field, &FieldValue::first);
    }

    static void normalizeBatchRows(std::vector<BatchRow>& rows,
                                   const std::string& keyField)
    {
        for (auto& row : rows)
        {
            std::vector<FieldValue> normalized;
            normalized.reserve(row.values.size());
            for (auto& [field, value] : row.values)
            {
                detail::requireIdentifier(field, "批量更新字段");
                if (field == keyField)
                {
                    throw std::invalid_argument(
                        "批量更新不能修改主键字段: " + keyField);
                }

                const auto existing = findField(normalized, field);
                if (existing == normalized.end())
                {
                    normalized.emplace_back(std::move(field),
                                            std::move(value));
                }
                else
                {
                    existing->second = std::move(value);
                }
            }
            row.values = std::move(normalized);
        }

        std::erase_if(rows, [](const BatchRow& row) {
            return row.values.empty();
        });

        std::vector<BatchRow> uniqueRows;
        uniqueRows.reserve(rows.size());
        for (auto& row : rows)
        {
            const auto existingRow = std::ranges::find(
                uniqueRows, row.key, &BatchRow::key);
            if (existingRow == uniqueRows.end())
            {
                uniqueRows.push_back(std::move(row));
                continue;
            }

            for (auto& [field, value] : row.values)
            {
                const auto existingField = findField(existingRow->values,
                                                     field);
                if (existingField == existingRow->values.end())
                {
                    existingRow->values.emplace_back(std::move(field),
                                                     std::move(value));
                }
                else
                {
                    existingField->second = std::move(value);
                }
            }
        }
        rows = std::move(uniqueRows);
    }

    SqlUpdate& setAssignment(std::string field,
                             std::string expression,
                             std::vector<SqlValue> values)
    {
        detail::requireIdentifier(field, "更新字段");
        const auto existing = std::ranges::find(
            assignments_, field, &Assignment::field);
        if (existing == assignments_.end())
        {
            assignments_.push_back(
                {std::move(field), std::move(expression), std::move(values)});
        }
        else
        {
            existing->expression = std::move(expression);
            existing->values = std::move(values);
        }
        return *this;
    }

    void validateExecutable() const
    {
        if (assignments_.empty())
        {
            throw std::invalid_argument("UPDATE 至少需要一个 SET 字段");
        }
        if (!hasWhere())
        {
            throw std::invalid_argument("拒绝执行没有 WHERE 条件的 UPDATE");
        }
    }

    drogon::orm::DbClientPtr client_;
    std::string table_;
    std::vector<Assignment> assignments_;
    std::size_t limit_{0};
};
}  // namespace sql
