//
// Created by 天之王·彦 on 2022/1/4.
//
#include "redisUtils.h"
#include <drogon/drogon.h>
#include <memory>
#include <string>

namespace {
    // 验证Redis键的有效性，防止注入攻击
    inline bool isValidRedisKey(const std::string& key) {
        if (key.empty() || key.length() > 1024) return false;
        // 检查是否包含控制字符
        for (char c : key) {
            if (c == '\n' || c == '\r' || c == '\0') {
                return false;
            }
        }
        return true;
    }

    // 转义Redis值中的控制字符
    inline std::string sanitizeRedisValue(const std::string& value) {
        std::string sanitized = value;
        // 移除或替换控制字符
        for (auto& c : sanitized) {
            if (c == '\n' || c == '\r' || c == '\0') {
                c = ' '; // 替换为普通空格
            }
        }
        return sanitized;
    }
}

drogon::Task<std::string> redisUtils::getCoroRedisValue(const std::string& key)
{
    if (!isValidRedisKey(key)) {
        LOG_ERROR << "Invalid Redis key provided: " << key;
        co_return "";
    }

    const auto redisClient = drogon::app().getFastRedisClient();
    try {
        const auto result = co_await redisClient->execCommandCoro(std::format("get {}", key));
        co_return result.isNil() ? "" : result.asString();
    } catch (const std::exception& e) {
        LOG_ERROR << "Redis GET failed for key '" << key << "': " << e.what();
        co_return "";
    }
}

drogon::Task<long> redisUtils::ttlCoroRedisKey(const std::string& key)
{
    if (!isValidRedisKey(key)) {
        LOG_ERROR << "Invalid Redis key provided: " << key;
        co_return -3; // 自定义异常值
    }

    const auto redisClient = drogon::app().getFastRedisClient();
    try {
        const auto result = co_await redisClient->execCommandCoro(std::format("ttl {}", key));
        co_return result.asInteger();  // 返回剩余秒数，-1表示永久，-2表示不存在
    } catch (const std::exception& e) {
        LOG_ERROR << "Redis TTL failed for key '" << key << "': " << e.what();
        co_return -3;  // 自定义异常值
    }
}

drogon::Task<bool> redisUtils::setCoroRedisValue(const std::string& key, const std::string& value)
{
    if (!isValidRedisKey(key)) {
        LOG_ERROR << "Invalid Redis key provided: " << key;
        co_return false;
    }

    const auto redisClient = drogon::app().getFastRedisClient();
    try {
        const auto sanitizedValue = sanitizeRedisValue(value);
        const auto result = co_await redisClient->execCommandCoro(std::format("set {} {}", key, sanitizedValue));
        co_return result.asString() == "OK";
    } catch (const std::exception& e) {
        LOG_ERROR << "Redis SET failed for key '" << key << "': " << e.what();
        co_return false;
    }
}

drogon::Task<bool> redisUtils::setExCoroRedisValue(const std::string& key, const int seconds, const std::string& value)
{
    if (!isValidRedisKey(key)) {
        LOG_ERROR << "Invalid Redis key provided: " << key;
        co_return false;
    }

    if (seconds <= 0) {
        LOG_ERROR << "Invalid expiration time: " << seconds;
        co_return false;
    }

    const auto redisClient = drogon::app().getFastRedisClient();
    try {
        const auto sanitizedValue = sanitizeRedisValue(value);
        const auto result = co_await redisClient->execCommandCoro(std::format("setex {} {} {}", key, std::to_string(seconds), sanitizedValue));
        co_return result.asString() == "OK";
    } catch (const std::exception& e) {
        LOG_ERROR << "Redis SETEX failed for key '" << key << "', ttl " << seconds << "': " << e.what();
        co_return false;
    }
}

drogon::Task<bool> redisUtils::delCoroRedisKey(const std::string& key)
{
    if (!isValidRedisKey(key)) {
        LOG_ERROR << "Invalid Redis key provided: " << key;
        co_return false;
    }

    const auto redisClient = drogon::app().getFastRedisClient();
    try {
        const auto result = co_await redisClient->execCommandCoro(std::format("del {}", key));
        co_return result.asInteger() > 0;
    } catch (const std::exception& e) {
        LOG_ERROR << "Redis DEL failed for key '" << key << "': " << e.what();
        co_return false;
    }
}

drogon::Task<bool> redisUtils::existsCoroRedisKey(const std::string& key)
{
    if (!isValidRedisKey(key)) {
        LOG_ERROR << "Invalid Redis key provided: " << key;
        co_return false;
    }

    const auto redisClient = drogon::app().getFastRedisClient();
    try {
        const auto result = co_await redisClient->execCommandCoro(std::format("exists {}", key));
        co_return result.asInteger() > 0;
    } catch (const std::exception& e) {
        LOG_ERROR << "Redis EXISTS failed for key '" << key << "': " << e.what();
        co_return false;
    }
}