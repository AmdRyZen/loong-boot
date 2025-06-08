#ifndef LEARNING_CPP_SQLFILTER_H
#define LEARNING_CPP_SQLFILTER_H

#pragma once

#include <string>
#include <variant>
#include <sstream>
#include <vector>
#include <utility>
#include <optional>

enum class LogicOp {
    And,
    Or
};

struct Condition {
    std::string field;
    std::string op;
    std::variant<std::string, int, long long, double> value;
    LogicOp logic = LogicOp::And; // 默认为 AND
    bool openParen = false;
    bool closeParen = false;
};

class SqlFilter {
public:
    // 判断字符串是否包含 SQL 注入关键词
    static bool FilteredSQLInject(const std::string& str) {
        static const std::vector<std::string> sql_keywords = {
            "select",
            "insert",
            "update",
            "delete",
            "drop",
            "--",
            ";",
            "/*",
            "*/",
            "xp_"};

        const std::string lowerStr = ToLower(str);
        return std::ranges::any_of(sql_keywords, [&](const std::string& keyword) {
            return lowerStr.find(keyword) != std::string::npos;
        });
    }

    // 将 variant 类型转为 string（用于输出）
    static std::string VariantToString(const std::variant<std::string, int, long long, double>& var) {
        return std::visit([](const auto& val) -> std::string {
            std::ostringstream oss;
            oss << val;
            return oss.str();
        }, var);
    }

    // 生成单条 SQL 条件，带防注入检查（不含 AND 前缀）
    static std::string BuildConditionSQL(const Condition& condition) {
        const std::string valueStr = VariantToString(condition.value);

        if (std::holds_alternative<std::string>(condition.value)) {
            if (FilteredSQLInject(std::get<std::string>(condition.value))) {
                throw std::runtime_error("SQL 注入风险: " + std::get<std::string>(condition.value));
            }
        }

        std::ostringstream oss;
        oss << condition.field << " " << condition.op << " '" << valueStr << "'";
        return oss.str();
    }

    static std::string BuildConditionsSQL(const std::vector<Condition>& conditions) {
        if (conditions.empty()) {
            return "";
        }

        std::string result;
        result.reserve(256);  // 预分配内存以优化性能
        bool first = true;

        for (const auto& cond : conditions) {
            std::string condSql = BuildConditionSQL(cond);
            if (first) {
                result += " WHERE " + condSql;
                first = false;
            } else {
                result += " AND " + condSql;
            }
        }

        return result;
    }

    static std::string BuildConditionsSQLWithParams(const std::vector<Condition>& conditions) {
        if (conditions.empty()) {
            return "";
        }

        std::string result;
        result.reserve(256);
        bool first = true;

        for (const auto& [field, op, value, logic, openParen, closeParen] : conditions) {
            const std::string valStr = VariantToString(value);

            if (std::holds_alternative<std::string>(value)) {
                if (FilteredSQLInject(std::get<std::string>(value))) {
                    throw std::runtime_error("SQL 注入风险: " + std::get<std::string>(value));
                }
            }

            if (!first) {
                result += logic == LogicOp::And ? " AND " : " OR ";
            } else {
                result += " WHERE ";
                first = false;
            }

            if (openParen) result += "(";
            std::ostringstream oss;
            oss << field << " " << op << " '" << valStr << "'";
            result += oss.str();
            if (closeParen) result += ")";
        }

        return result;
    }

    struct ConditionExpr {
        // 叶子节点：单个条件
        std::optional<Condition> condition;
        // 非叶子节点：逻辑操作
        std::vector<ConditionExpr> children;
        LogicOp logic = LogicOp::And;
        // 是否用括号包裹此节点
        bool grouped = false;
    };

    // 新增：从表达式树构建 SQL 及参数
    static std::string BuildSQLFromExprTreeWithValues(const ConditionExpr& expr, const bool isTopLevel = true) {
        if (expr.condition.has_value()) {
            const auto& cond = *expr.condition;
            const std::string valStr = SqlFilter::VariantToString(cond.value);

            if (std::holds_alternative<std::string>(cond.value)) {
                if (SqlFilter::FilteredSQLInject(std::get<std::string>(cond.value))) {
                    throw std::runtime_error("SQL 注入风险: " + std::get<std::string>(cond.value));
                }
                return cond.field + " " + cond.op + " '" + valStr + "'";
            }
            return cond.field + " " + cond.op + " " + valStr;

        }

        std::string result;
        bool first = true;
        for (const auto& child : expr.children) {
            std::string childSQL = BuildSQLFromExprTreeWithValues(child, false);
            if (child.grouped) {
                childSQL.insert(childSQL.begin(), '(');
                childSQL.push_back(')');
            }
            if (!first) {
                if (expr.logic == LogicOp::And) {
                    result.append(" AND ");
                } else {
                    result.append(" OR ");
                }
            }
            result.append(childSQL);
            first = false;
        }

        if (!result.empty() && isTopLevel) {
            result = " WHERE " + result;
        }

        return result;
    }

private:
    static std::string ToLower(const std::string& input) {
        std::string result = input;
        for (char& c : result) {
            c = static_cast<char>(tolower(c));
        }
        return result;
    }
};

#endif