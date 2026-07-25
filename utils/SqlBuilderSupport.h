#pragma once

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include <algorithm>
#include <concepts>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sql
{
class SqlValue
{
  public:
    using Binary = std::vector<char>;
    using Storage = std::variant<std::nullptr_t,
                                 bool,
                                 std::int64_t,
                                 std::uint64_t,
                                 double,
                                 std::string,
                                 Binary>;

    SqlValue() : value_(nullptr)
    {
    }

    SqlValue(std::nullptr_t) : value_(nullptr)
    {
    }

    SqlValue(bool value) : value_(value)
    {
    }

    template <std::signed_integral T>
        requires(!std::same_as<std::remove_cv_t<T>, bool>)
    SqlValue(T value) : value_(static_cast<std::int64_t>(value))
    {
    }

    template <std::unsigned_integral T>
        requires(!std::same_as<std::remove_cv_t<T>, bool>)
    SqlValue(T value) : value_(static_cast<std::uint64_t>(value))
    {
    }

    template <std::floating_point T>
    SqlValue(T value) : value_(static_cast<double>(value))
    {
    }

    SqlValue(const char* value)
        : value_(value == nullptr ? Storage{nullptr}
                                  : Storage{std::string(value)})
    {
    }

    SqlValue(std::string_view value) : value_(std::string(value))
    {
    }

    SqlValue(const std::string& value) : value_(value)
    {
    }

    SqlValue(std::string&& value) : value_(std::move(value))
    {
    }

    SqlValue(const Binary& value) : value_(value)
    {
    }

    SqlValue(Binary&& value) : value_(std::move(value))
    {
    }

    template <typename T>
    SqlValue(const std::optional<T>& value)
        : SqlValue(value ? SqlValue(*value) : SqlValue(nullptr))
    {
    }

    SqlValue(const Json::Value& value)
    {
        switch (value.type())
        {
            case Json::nullValue:
                value_ = nullptr;
                break;
            case Json::intValue:
                value_ = value.asInt64();
                break;
            case Json::uintValue:
                value_ = value.asUInt64();
                break;
            case Json::realValue:
                value_ = value.asDouble();
                break;
            case Json::stringValue:
                value_ = value.asString();
                break;
            case Json::booleanValue:
                value_ = value.asBool();
                break;
            case Json::arrayValue:
            case Json::objectValue:
            default:
            {
                Json::StreamWriterBuilder builder;
                builder["indentation"] = "";
                value_ = Json::writeString(builder, value);
                break;
            }
        }
    }

    void bind(drogon::orm::internal::SqlBinder& binder) const
    {
        std::visit(
            [&binder](const auto& value) {
                using T = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<T, std::nullptr_t>)
                {
                    binder << nullptr;
                }
                else
                {
                    binder << value;
                }
            },
            value_);
    }

    bool operator==(const SqlValue&) const = default;

  private:
    Storage value_;
};

namespace detail
{
template <std::size_t N>
struct FixedString
{
    char value[N]{};

    consteval FixedString(const char (&text)[N])
    {
        for (std::size_t i = 0; i < N; ++i)
        {
            value[i] = text[i];
        }
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept
    {
        return {value, N - 1};
    }
};

enum class SqlConnector
{
    And,
    Or
};

struct WhereClause
{
    SqlConnector connector{SqlConnector::And};
    std::string condition;
};

inline bool isIdentifier(std::string_view value)
{
    if (value.empty())
    {
        return false;
    }

    bool atSegmentStart = true;
    for (const unsigned char character : value)
    {
        if (character == '.')
        {
            if (atSegmentStart)
            {
                return false;
            }
            atSegmentStart = true;
            continue;
        }

        if (atSegmentStart)
        {
            if (!std::isalpha(character) && character != '_')
            {
                return false;
            }
            atSegmentStart = false;
            continue;
        }

        if (!std::isalnum(character) && character != '_')
        {
            return false;
        }
    }
    return !atSegmentStart;
}

inline void requireIdentifier(std::string_view value,
                              std::string_view description)
{
    if (!isIdentifier(value))
    {
        throw std::invalid_argument("无效的" + std::string(description) +
                                    ": " + std::string(value));
    }
}

constexpr std::size_t placeholderCount(std::string_view sql)
{
    enum class State
    {
        Normal,
        SingleQuote,
        DoubleQuote,
        Backtick,
        LineComment,
        BlockComment
    };

    State state = State::Normal;
    std::size_t count = 0;

    for (std::size_t i = 0; i < sql.size(); ++i)
    {
        const char character = sql[i];
        const char next = i + 1 < sql.size() ? sql[i + 1] : '\0';

        switch (state)
        {
            case State::Normal:
                if (character == '\'')
                    state = State::SingleQuote;
                else if (character == '"')
                    state = State::DoubleQuote;
                else if (character == '`')
                    state = State::Backtick;
                else if (character == '-' && next == '-')
                {
                    state = State::LineComment;
                    ++i;
                }
                else if (character == '#')
                    state = State::LineComment;
                else if (character == '/' && next == '*')
                {
                    state = State::BlockComment;
                    ++i;
                }
                else if (character == '?')
                    ++count;
                break;

            case State::SingleQuote:
                if (character == '\\')
                    ++i;
                else if (character == '\'' && next == '\'')
                    ++i;
                else if (character == '\'')
                    state = State::Normal;
                break;

            case State::DoubleQuote:
                if (character == '\\')
                    ++i;
                else if (character == '"' && next == '"')
                    ++i;
                else if (character == '"')
                    state = State::Normal;
                break;

            case State::Backtick:
                if (character == '`' && next == '`')
                    ++i;
                else if (character == '`')
                    state = State::Normal;
                break;

            case State::LineComment:
                if (character == '\n' || character == '\r')
                    state = State::Normal;
                break;

            case State::BlockComment:
                if (character == '*' && next == '/')
                {
                    state = State::Normal;
                    ++i;
                }
                break;
        }
    }

    return count;
}

inline void validateWhere(std::string_view condition,
                          std::size_t argumentCount)
{
    if (condition.empty() || condition.find(';') != std::string_view::npos)
    {
        throw std::invalid_argument("无效的 WHERE 条件");
    }
    if (placeholderCount(condition) != argumentCount)
    {
        throw std::invalid_argument("WHERE 条件中的 ? 数量与参数数量不一致");
    }
}

inline void appendConditions(std::string& statement,
                             std::string_view keyword,
                             const std::vector<WhereClause>& clauses)
{
    if (clauses.empty())
    {
        return;
    }

    statement += keyword;
    for (std::size_t i = 0; i < clauses.size(); ++i)
    {
        if (i > 0)
        {
            statement += clauses[i].connector == SqlConnector::Or
                             ? " or "
                             : " and ";
        }
        statement += '(';
        statement += clauses[i].condition;
        statement += ')';
    }
}

inline auto execute(const drogon::orm::DbClientPtr& client,
                    std::string statement,
                    const std::vector<SqlValue>& parameters)
{
    auto binder = client->operator<<(std::move(statement));
    for (const auto& parameter : parameters)
    {
        parameter.bind(binder);
    }
    return drogon::orm::internal::SqlAwaiter(std::move(binder));
}

template <typename BindParameters>
inline auto executeWith(const drogon::orm::DbClientPtr& client,
                        std::string statement,
                        BindParameters&& bindParameters)
{
    auto binder = client->operator<<(std::move(statement));
    std::forward<BindParameters>(bindParameters)(binder);
    return drogon::orm::internal::SqlAwaiter(std::move(binder));
}

template <typename Derived>
class WhereBuilder
{
  public:
    template <FixedString Condition, typename... Args>
    Derived& where(Args&&... args)
    {
        constexpr auto condition = Condition.view();
        static_assert(!condition.empty(), "WHERE 条件不能为空");
        static_assert(condition.find(';') == std::string_view::npos,
                      "WHERE 条件不能包含分号");
        static_assert(placeholderCount(condition) == sizeof...(Args),
                      "WHERE 条件中的 ? 数量与参数数量不一致");
        return addWhereUnchecked(SqlConnector::And,
                                 std::string(condition),
                                 std::forward<Args>(args)...);
    }

    template <typename... Args>
    Derived& where(std::string condition, Args&&... args)
    {
        return addWhere(SqlConnector::And,
                        std::move(condition),
                        std::forward<Args>(args)...);
    }

    template <FixedString Condition, typename... Args>
    Derived& orWhere(Args&&... args)
    {
        constexpr auto condition = Condition.view();
        static_assert(!condition.empty(), "WHERE 条件不能为空");
        static_assert(condition.find(';') == std::string_view::npos,
                      "WHERE 条件不能包含分号");
        static_assert(placeholderCount(condition) == sizeof...(Args),
                      "WHERE 条件中的 ? 数量与参数数量不一致");
        return addWhereUnchecked(SqlConnector::Or,
                                 std::string(condition),
                                 std::forward<Args>(args)...);
    }

    template <typename... Args>
    Derived& orWhere(std::string condition, Args&&... args)
    {
        return addWhere(SqlConnector::Or,
                        std::move(condition),
                        std::forward<Args>(args)...);
    }

    template <typename... Args>
    Derived& whereIf(bool enabled, std::string condition, Args&&... args)
    {
        if (enabled)
        {
            where(std::move(condition), std::forward<Args>(args)...);
        }
        return self();
    }

    template <typename T>
    Derived& whereEq(std::string field, T&& value)
    {
        return compare(std::move(field), " = ?", std::forward<T>(value));
    }

    template <typename T>
    Derived& whereNe(std::string field, T&& value)
    {
        return compare(std::move(field), " != ?", std::forward<T>(value));
    }

    template <typename T>
    Derived& whereGt(std::string field, T&& value)
    {
        return compare(std::move(field), " > ?", std::forward<T>(value));
    }

    template <typename T>
    Derived& whereGe(std::string field, T&& value)
    {
        return compare(std::move(field), " >= ?", std::forward<T>(value));
    }

    template <typename T>
    Derived& whereLt(std::string field, T&& value)
    {
        return compare(std::move(field), " < ?", std::forward<T>(value));
    }

    template <typename T>
    Derived& whereLe(std::string field, T&& value)
    {
        return compare(std::move(field), " <= ?", std::forward<T>(value));
    }

    template <typename T>
    Derived& whereLike(std::string field, T&& value)
    {
        return compare(std::move(field), " like ?", std::forward<T>(value));
    }

    template <typename T>
    Derived& orWhereEq(std::string field, T&& value)
    {
        requireIdentifier(field, "WHERE 字段");
        field += " = ?";
        return orWhere(std::move(field), std::forward<T>(value));
    }

    template <typename Lower, typename Upper>
    Derived& whereBetween(std::string field,
                          Lower&& lower,
                          Upper&& upper)
    {
        requireIdentifier(field, "WHERE 字段");
        field += " between ? and ?";
        return where(std::move(field),
                     std::forward<Lower>(lower),
                     std::forward<Upper>(upper));
    }

    template <typename Lower, typename Upper>
    Derived& whereNotBetween(std::string field,
                             Lower&& lower,
                             Upper&& upper)
    {
        requireIdentifier(field, "WHERE 字段");
        field += " not between ? and ?";
        return where(std::move(field),
                     std::forward<Lower>(lower),
                     std::forward<Upper>(upper));
    }

    template <typename T>
    Derived& whereIfPresent(std::string condition,
                            const std::optional<T>& value)
    {
        if (value)
        {
            where(std::move(condition), *value);
        }
        return self();
    }

    Derived& whereNull(std::string field)
    {
        requireIdentifier(field, "WHERE 字段");
        return where(std::move(field) + " is null");
    }

    Derived& whereNotNull(std::string field)
    {
        requireIdentifier(field, "WHERE 字段");
        return where(std::move(field) + " is not null");
    }

    template <std::ranges::input_range Range>
        requires(!std::convertible_to<Range, std::string_view>)
    Derived& whereIn(std::string field, const Range& values)
    {
        return addIn(SqlConnector::And, std::move(field), values, false);
    }

    template <std::ranges::input_range Range>
        requires(!std::convertible_to<Range, std::string_view>)
    Derived& whereNotIn(std::string field, const Range& values)
    {
        return addIn(SqlConnector::And, std::move(field), values, true);
    }

    template <std::ranges::input_range Range>
        requires(!std::convertible_to<Range, std::string_view>)
    Derived& orWhereIn(std::string field, const Range& values)
    {
        return addIn(SqlConnector::Or, std::move(field), values, false);
    }

    [[nodiscard]] bool hasWhere() const noexcept
    {
        return !whereClauses_.empty();
    }

  protected:
    void appendWhere(std::string& statement) const
    {
        appendConditions(statement, " where ", whereClauses_);
    }

    [[nodiscard]] std::size_t whereSqlCapacity() const noexcept
    {
        std::size_t capacity = 0;
        for (const auto& clause : whereClauses_)
        {
            capacity += clause.condition.size() + 8;
        }
        return capacity;
    }

    std::vector<WhereClause> whereClauses_;
    std::vector<SqlValue> whereValues_;

  private:
    Derived& self() noexcept
    {
        return static_cast<Derived&>(*this);
    }

    template <typename... Args>
    Derived& addWhere(SqlConnector connector,
                      std::string condition,
                      Args&&... args)
    {
        validateWhere(condition, sizeof...(Args));
        return addWhereUnchecked(connector,
                                 std::move(condition),
                                 std::forward<Args>(args)...);
    }

    template <typename... Args>
    Derived& addWhereUnchecked(SqlConnector connector,
                               std::string condition,
                               Args&&... args)
    {
        whereClauses_.push_back({connector, std::move(condition)});
        (whereValues_.emplace_back(std::forward<Args>(args)), ...);
        return self();
    }

    template <typename T>
    Derived& compare(std::string field, std::string_view operation, T&& value)
    {
        requireIdentifier(field, "WHERE 字段");
        field += operation;
        return where(std::move(field), std::forward<T>(value));
    }

    template <std::ranges::input_range Range>
    Derived& addIn(SqlConnector connector,
                   std::string field,
                   const Range& values,
                   bool negated)
    {
        requireIdentifier(field, "WHERE 字段");

        std::vector<SqlValue> parameters;
        if constexpr (std::ranges::sized_range<Range>)
        {
            parameters.reserve(std::ranges::size(values));
        }
        for (const auto& value : values)
        {
            parameters.emplace_back(value);
        }

        if (parameters.empty())
        {
            if (negated && Derived::rejectEmptyNotIn)
            {
                throw std::invalid_argument(
                    "UPDATE 的 NOT IN 参数不能为空，避免误更新整表");
            }
            return addWhere(connector, negated ? "1 = 1" : "1 = 0");
        }

        std::string condition = std::move(field);
        condition += negated ? " not in (" : " in (";
        for (std::size_t i = 0; i < parameters.size(); ++i)
        {
            if (i > 0)
            {
                condition += ", ";
            }
            condition += '?';
        }
        condition += ')';

        whereClauses_.push_back({connector, std::move(condition)});
        whereValues_.insert(whereValues_.end(),
                            std::make_move_iterator(parameters.begin()),
                            std::make_move_iterator(parameters.end()));
        return self();
    }
};
}  // namespace detail
}  // namespace sql
