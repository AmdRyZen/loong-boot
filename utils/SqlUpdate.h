#pragma once

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include <cctype>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sql
{
class SqlUpdate
{
  public:
    using FieldValue = std::pair<std::string, Json::Value>;

    SqlUpdate(drogon::orm::DbClientPtr client, std::string table)
        : client_(std::move(client)), table_(std::move(table))
    {
        if (!client_ || !isIdentifier(table_))
        {
            throw std::invalid_argument("无效的数据库客户端或表名");
        }
    }

    SqlUpdate& set(std::initializer_list<FieldValue> values)
    {
        for (const auto& [field, value] : values)
        {
            if (!isIdentifier(field))
            {
                throw std::invalid_argument("无效的更新字段: " + field);
            }
            setFields_.push_back(field);
            setValues_.push_back(value);
        }
        return *this;
    }

    template <typename... Args>
    SqlUpdate& where(std::string condition, Args&&... args)
    {
        if (condition.empty() || condition.find(';') != std::string::npos)
        {
            throw std::invalid_argument("无效的 WHERE 条件");
        }
        if (placeholderCount(condition) != sizeof...(Args))
        {
            throw std::invalid_argument("WHERE 条件中的 ? 数量与参数数量不一致");
        }

        whereClauses_.push_back(std::move(condition));
        (whereValues_.emplace_back(std::forward<Args>(args)), ...);
        return *this;
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

    auto exec()
    {
        if (setFields_.empty())
        {
            throw std::invalid_argument("UPDATE 至少需要一个 SET 字段");
        }
        if (whereClauses_.empty())
        {
            throw std::invalid_argument("拒绝执行没有 WHERE 条件的 UPDATE");
        }

        std::string statement = "update " + table_ + " set ";
        std::vector<Json::Value> parameters;
        parameters.reserve(setValues_.size() + whereValues_.size());

        for (std::size_t i = 0; i < setFields_.size(); ++i)
        {
            if (i > 0)
            {
                statement += ", ";
            }
            statement += setFields_[i] + " = ?";
            parameters.push_back(setValues_[i]);
        }

        statement += " where ";
        for (std::size_t i = 0; i < whereClauses_.size(); ++i)
        {
            if (i > 0)
            {
                statement += " and ";
            }
            statement += '(' + whereClauses_[i] + ')';
        }
        parameters.insert(parameters.end(), whereValues_.begin(), whereValues_.end());

        if (limit_ > 0)
        {
            statement += " limit " + std::to_string(limit_);
        }
        return client_->execSqlCoro(
            statement,
            static_cast<const std::vector<Json::Value>&>(parameters));
    }

  private:
    static bool isIdentifier(std::string_view value)
    {
        if (value.empty())
        {
            return false;
        }
        for (const unsigned char character : value)
        {
            if (!std::isalnum(character) && character != '_')
            {
                return false;
            }
        }
        return true;
    }

    static std::size_t placeholderCount(std::string_view condition)
    {
        std::size_t count = 0;
        for (const auto character : condition)
        {
            count += character == '?';
        }
        return count;
    }

    drogon::orm::DbClientPtr client_;
    std::string table_;
    std::vector<std::string> setFields_;
    std::vector<Json::Value> setValues_;
    std::vector<std::string> whereClauses_;
    std::vector<Json::Value> whereValues_;
    std::size_t limit_{0};
};
}  // namespace sql
