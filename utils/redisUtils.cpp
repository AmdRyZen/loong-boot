//
// Created by 天使之王·彦 on 2022/1/4.
//
#include "redisUtils.h"
#include <drogon/drogon.h>
#include <memory>

drogon::Task<std::string> redisUtils::getCoroRedisValue(const std::string& key)
{
    const auto redisClient = drogon::app().getFastRedisClient();
    try {
        const auto result = co_await redisClient->execCommandCoro(std::format("get {}", key));
        co_return result.isNil() ? "" : result.asString();
    } catch (const std::exception& e) {
        LOG_ERROR << "Redis GET failed: " << e.what();
        co_return "";
    }
}

drogon::Task<long> redisUtils::ttlCoroRedisKey(const std::string& key)
{
    const auto redisClient = drogon::app().getFastRedisClient();
    try {
        const auto result = co_await redisClient->execCommandCoro(std::format("ttl {}", key));
        co_return result.asInteger();  // 返回剩余秒数，-1表示永久，-2表示不存在
    } catch (const std::exception& e) {
        LOG_ERROR << "Redis TTL failed: " << e.what();
        co_return -3;  // 自定义异常值
    }
}

drogon::Task<bool> redisUtils::setCoroRedisValue(const std::string& key, const std::string& value)
{
    const auto redisClient = drogon::app().getFastRedisClient();
    try {
        const auto result = co_await redisClient->execCommandCoro( std::format("set {} {}", key, value));
        co_return result.asString() == "OK";
    } catch (const std::exception& e) {
        LOG_ERROR << "Redis SET failed: " << e.what();
        co_return false;
    }
}

drogon::Task<bool> redisUtils::setExCoroRedisValue(const std::string& key, const int seconds, const std::string& value)
{
    const auto redisClient = drogon::app().getFastRedisClient();
    try {
        const auto result = co_await redisClient->execCommandCoro(std::format("setex {} {} {}", key, std::to_string(seconds), value));
        co_return result.asString() == "OK";
    } catch (const std::exception& e) {
        LOG_ERROR << "Redis SETEX failed: " << e.what();
        co_return false;
    }
}

drogon::Task<bool> redisUtils::delCoroRedisKey(const std::string& key)
{
    const auto redisClient = drogon::app().getFastRedisClient();
    try {
        const auto result = co_await redisClient->execCommandCoro(std::format("del {}", key));
        co_return result.asInteger() > 0;
    } catch (const std::exception& e) {
        LOG_ERROR << "Redis DEL failed: " << e.what();
        co_return false;
    }
}

drogon::Task<bool> redisUtils::existsCoroRedisKey(const std::string& key)
{
    const auto redisClient = drogon::app().getFastRedisClient();
    try {
        const auto result = co_await redisClient->execCommandCoro(std::format("exists {}", key));
        co_return result.asInteger() > 0;
    } catch (const std::exception& e) {
        LOG_ERROR << "Redis EXISTS failed: " << e.what();
        co_return false;
    }
}
