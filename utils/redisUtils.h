//
// Created by 天使之王·彦 on 2022/1/4.
//

#ifndef DROGON_HTTP_REDISUTILS_H
#define DROGON_HTTP_REDISUTILS_H

#include <string>
#include <drogon/drogon.h>

class redisUtils
{
  public:
    static drogon::Task<std::string> getCoroRedisValue(const std::string& key);
    static drogon::Task<long> ttlCoroRedisKey(const std::string& key);
    static drogon::Task<bool> setCoroRedisValue(const std::string& key, const std::string& value);
    static drogon::Task<bool> setExCoroRedisValue(const std::string& key, int seconds, const std::string& value);
    static drogon::Task<bool> delCoroRedisKey(const std::string& key);
    static drogon::Task<bool> existsCoroRedisKey(const std::string& key);
};

#endif  //DROGON_HTTP_REDISUTILS_H
