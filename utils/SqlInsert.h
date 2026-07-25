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
class SqlInsert
{
  public:
    using FieldValue = std::pair<std::string, SqlValue>;
    using Row = std::vector<FieldValue>;

    struct BatchOptions
    {
        std::size_t batchSize{500};
        bool ignore{false};
        std::vector<std::string> updateFields;
    };

    struct PreparedBatch
    {
        std::string statement;
        std::vector<SqlValue> parameters;
        std::size_t rowCount{};
    };

    SqlInsert(drogon::orm::DbClientPtr client, std::string table)
        : client_(std::move(client)), table_(std::move(table))
    {
        if (!client_)
        {
            throw std::invalid_argument("无效的数据库客户端");
        }
        detail::requireIdentifier(table_, "表名");
    }

    SqlInsert& values(std::initializer_list<FieldValue> values)
    {
        for (const auto& [field, value] : values)
        {
            this->value(field, value);
        }
        return *this;
    }

    template <typename T>
    SqlInsert& value(std::string field, T&& value)
    {
        detail::requireIdentifier(field, "插入字段");
        const auto existing = findField(values_, field);
        if (existing == values_.end())
        {
            values_.emplace_back(std::move(field),
                                 SqlValue(std::forward<T>(value)));
        }
        else
        {
            existing->second = SqlValue(std::forward<T>(value));
        }
        return *this;
    }

    SqlInsert& ignore(bool enabled = true) noexcept
    {
        ignore_ = enabled;
        return *this;
    }

    SqlInsert& onDuplicateUpdate(
        std::initializer_list<std::string> fields)
    {
        updateFields_.clear();
        updateFields_.reserve(fields.size());
        for (const auto& field : fields)
        {
            detail::requireIdentifier(field, "Upsert 字段");
            if (findField(values_, field) == values_.end())
            {
                throw std::invalid_argument(
                    "Upsert 字段不在插入数据中: " + field);
            }
            if (std::ranges::find(updateFields_, field) ==
                updateFields_.end())
            {
                updateFields_.push_back(field);
            }
        }
        return *this;
    }

    [[nodiscard]] std::string toSql() const
    {
        validateValues(values_);
        std::vector<std::string> fields;
        fields.reserve(values_.size());
        for (const auto& [field, value] : values_)
        {
            static_cast<void>(value);
            fields.push_back(field);
        }
        return buildStatement(table_, fields, 1, ignore_, updateFields_);
    }

    auto exec() const
    {
        return detail::executeWith(
            client_,
            toSql(),
            [this](drogon::orm::internal::SqlBinder& binder) {
                for (const auto& [field, value] : values_)
                {
                    static_cast<void>(field);
                    value.bind(binder);
                }
            });
    }

    static PreparedBatch prepareBatch(std::string table,
                                      std::vector<Row> rows,
                                      BatchOptions options)
    {
        detail::requireIdentifier(table, "表名");
        normalizeRows(rows);
        if (rows.empty())
        {
            throw std::invalid_argument("批量插入至少需要一条数据");
        }

        const auto fields = collectFields(rows);
        normalizeUpdateFields(options.updateFields, fields);
        return prepareNormalized(std::move(table),
                                 rows,
                                 fields,
                                 options.ignore,
                                 options.updateFields);
    }

    static PreparedBatch prepareBatch(std::string table,
                                      std::vector<Row> rows)
    {
        return prepareBatch(std::move(table),
                            std::move(rows),
                            BatchOptions{});
    }

    static auto batchOnce(drogon::orm::DbClientPtr client,
                          std::string table,
                          std::vector<Row> rows,
                          BatchOptions options)
    {
        if (!client)
        {
            throw std::invalid_argument("无效的数据库客户端");
        }
        auto prepared = prepareBatch(std::move(table),
                                     std::move(rows),
                                     std::move(options));
        return detail::execute(client,
                               std::move(prepared.statement),
                               prepared.parameters);
    }

    static auto batchOnce(drogon::orm::DbClientPtr client,
                          std::string table,
                          std::vector<Row> rows)
    {
        return batchOnce(std::move(client),
                         std::move(table),
                         std::move(rows),
                         BatchOptions{});
    }

    static drogon::Task<std::uint64_t> batch(
        drogon::orm::DbClientPtr client,
        std::string table,
        std::vector<Row> rows,
        BatchOptions options)
    {
        if (!client)
        {
            throw std::invalid_argument("无效的数据库客户端");
        }
        if (options.batchSize == 0)
        {
            throw std::invalid_argument("批量插入分批大小必须大于 0");
        }

        detail::requireIdentifier(table, "表名");
        normalizeRows(rows);
        if (rows.empty())
        {
            throw std::invalid_argument("批量插入至少需要一条数据");
        }

        const auto fields = collectFields(rows);
        normalizeUpdateFields(options.updateFields, fields);

        std::uint64_t affectedRows = 0;
        for (std::size_t offset = 0; offset < rows.size();)
        {
            const auto end = offset + std::min(options.batchSize,
                                               rows.size() - offset);
            std::vector<Row> chunk;
            chunk.reserve(end - offset);
            for (std::size_t i = offset; i < end; ++i)
            {
                chunk.push_back(std::move(rows[i]));
            }

            auto prepared = prepareNormalized(table,
                                              chunk,
                                              fields,
                                              options.ignore,
                                              options.updateFields);
            const auto result = co_await detail::execute(
                client,
                std::move(prepared.statement),
                prepared.parameters);
            affectedRows += result.affectedRows();
            offset = end;
        }
        co_return affectedRows;
    }

    static drogon::Task<std::uint64_t> batch(
        drogon::orm::DbClientPtr client,
        std::string table,
        std::vector<Row> rows)
    {
        return batch(std::move(client),
                     std::move(table),
                     std::move(rows),
                     BatchOptions{});
    }

  private:
    using FieldIterator = Row::iterator;
    using ConstFieldIterator = Row::const_iterator;

    static FieldIterator findField(Row& row, const std::string& field)
    {
        return std::ranges::find(row, field, &FieldValue::first);
    }

    static ConstFieldIterator findField(const Row& row,
                                        const std::string& field)
    {
        return std::ranges::find(row, field, &FieldValue::first);
    }

    static void validateValues(const Row& values)
    {
        if (values.empty())
        {
            throw std::invalid_argument("INSERT 至少需要一个字段");
        }
    }

    static void normalizeRows(std::vector<Row>& rows)
    {
        for (auto& row : rows)
        {
            Row normalized;
            normalized.reserve(row.size());
            for (auto& [field, value] : row)
            {
                detail::requireIdentifier(field, "插入字段");
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
            row = std::move(normalized);
        }
        std::erase_if(rows, [](const Row& row) {
            return row.empty();
        });
    }

    static std::vector<std::string> collectFields(
        const std::vector<Row>& rows)
    {
        std::vector<std::string> fields;
        for (const auto& row : rows)
        {
            for (const auto& [field, value] : row)
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
            throw std::invalid_argument("批量插入至少需要一个字段");
        }
        return fields;
    }

    static void normalizeUpdateFields(
        std::vector<std::string>& updateFields,
        const std::vector<std::string>& insertFields)
    {
        std::vector<std::string> normalized;
        normalized.reserve(updateFields.size());
        for (const auto& field : updateFields)
        {
            detail::requireIdentifier(field, "Upsert 字段");
            if (std::ranges::find(insertFields, field) == insertFields.end())
            {
                throw std::invalid_argument(
                    "Upsert 字段不在批量插入字段中: " + field);
            }
            if (std::ranges::find(normalized, field) == normalized.end())
            {
                normalized.push_back(field);
            }
        }
        updateFields = std::move(normalized);
    }

    static std::string buildStatement(
        const std::string& table,
        const std::vector<std::string>& fields,
        std::size_t rowCount,
        bool ignore,
        const std::vector<std::string>& updateFields)
    {
        std::string statement;
        statement.reserve(48 + table.size() + fields.size() * 16 +
                          rowCount * fields.size() * 3 +
                          updateFields.size() * 24);
        statement = ignore ? "insert ignore into " : "insert into ";
        statement += table;
        statement += " (";
        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            if (i > 0)
                statement += ", ";
            statement += fields[i];
        }
        statement += ") values ";

        for (std::size_t row = 0; row < rowCount; ++row)
        {
            if (row > 0)
                statement += ", ";
            statement += '(';
            for (std::size_t field = 0; field < fields.size(); ++field)
            {
                if (field > 0)
                    statement += ", ";
                statement += '?';
            }
            statement += ')';
        }

        if (!updateFields.empty())
        {
            statement += " on duplicate key update ";
            for (std::size_t i = 0; i < updateFields.size(); ++i)
            {
                if (i > 0)
                    statement += ", ";
                statement += updateFields[i];
                statement += " = values(";
                statement += updateFields[i];
                statement += ')';
            }
        }
        return statement;
    }

    static PreparedBatch prepareNormalized(
        std::string table,
        const std::vector<Row>& rows,
        const std::vector<std::string>& fields,
        bool ignore,
        const std::vector<std::string>& updateFields)
    {
        std::vector<SqlValue> parameters;
        parameters.reserve(rows.size() * fields.size());
        for (const auto& row : rows)
        {
            for (const auto& field : fields)
            {
                const auto value = findField(row, field);
                if (value == row.end())
                    parameters.emplace_back(nullptr);
                else
                    parameters.push_back(value->second);
            }
        }

        return {
            buildStatement(table,
                           fields,
                           rows.size(),
                           ignore,
                           updateFields),
            std::move(parameters),
            rows.size()
        };
    }

    drogon::orm::DbClientPtr client_;
    std::string table_;
    Row values_;
    std::vector<std::string> updateFields_;
    bool ignore_{false};
};
}  // namespace sql
