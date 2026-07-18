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
enum class SqlOrder
{
    Asc,
    Desc
};

class SqlQuery
{
  public:
    SqlQuery(drogon::orm::DbClientPtr client, std::string table)
        : client_(std::move(client)), table_(std::move(table))
    {
        if (!client_ || !isIdentifier(table_))
        {
            throw std::invalid_argument("无效的数据库客户端或表名");
        }
    }

    SqlQuery& select(std::initializer_list<std::string> fields)
    {
        for (const auto& field : fields)
        {
            if (field != "*" && !isIdentifier(field))
            {
                throw std::invalid_argument("无效的查询字段: " + field);
            }
            selectFields_.push_back(field);
        }
        return *this;
    }

    SqlQuery& distinct()
    {
        distinct_ = true;
        return *this;
    }

    template <typename... Args>
    SqlQuery& where(std::string condition, Args&&... args)
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

    SqlQuery& orderBy(std::string field, SqlOrder order = SqlOrder::Asc)
    {
        if (!isIdentifier(field))
        {
            throw std::invalid_argument("无效的排序字段: " + field);
        }
        orderBy_ = std::move(field);
        order_ = order;
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

    SqlQuery& offset(std::size_t value)
    {
        offset_ = value;
        return *this;
    }

    auto exec()
    {
        std::string statement = "select ";
        if (distinct_)
        {
            statement += "distinct ";
        }

        if (selectFields_.empty())
        {
            statement += '*';
        }
        else
        {
            for (std::size_t i = 0; i < selectFields_.size(); ++i)
            {
                if (i > 0)
                {
                    statement += ", ";
                }
                statement += selectFields_[i];
            }
        }
        statement += " from " + table_;

        if (!whereClauses_.empty())
        {
            statement += " where ";
            for (std::size_t i = 0; i < whereClauses_.size(); ++i)
            {
                if (i > 0)
                {
                    statement += " and ";
                }
                statement += '(' + whereClauses_[i] + ')';
            }
        }

        if (!orderBy_.empty())
        {
            statement += " order by " + orderBy_;
            statement += order_ == SqlOrder::Desc ? " desc" : " asc";
        }
        if (limit_ > 0)
        {
            statement += " limit " + std::to_string(limit_);
        }
        if (offset_ > 0)
        {
            if (limit_ == 0)
            {
                throw std::invalid_argument("OFFSET 必须和 LIMIT 一起使用");
            }
            statement += " offset " + std::to_string(offset_);
        }

        return client_->execSqlCoro(
            statement,
            static_cast<const std::vector<Json::Value>&>(whereValues_));
    }

    template <typename T>
    drogon::Task<std::vector<T>> list() &
    {
        const auto result = co_await exec();
        std::vector<T> records;
        records.reserve(result.size());
        for (const auto& row : result)
        {
            records.push_back(T::from(row));
        }
        co_return records;
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
    std::vector<std::string> selectFields_;
    std::vector<std::string> whereClauses_;
    std::vector<Json::Value> whereValues_;
    std::string orderBy_;
    SqlOrder order_{SqlOrder::Asc};
    std::size_t limit_{0};
    std::size_t offset_{0};
    bool distinct_{false};
};
}  // namespace sql
