//
// Created by 神圣•凯莎 on 24-8-2.
//

#ifndef DATA_VO_H
#define DATA_VO_H

#include <string>
#include <tbb/concurrent_vector.h>
#include <drogon/orm/Result.h>
using namespace drogon; // 或者只用 using drogon::orm::Row;

// 定义 AesResponseDataVo 结构体
struct alignas(16) AesResponseDataVo {
    std::string encrypted;
    std::string decrypted;
    std::string hash;
    std::string md5_hash;
};

template <>
struct glz::meta<AesResponseDataVo> {
    using T = AesResponseDataVo;
    static constexpr auto value = object(
        "encrypted", &T::encrypted,
        "decrypted", &T::decrypted,
        "hash", &T::hash,
        "md5_hash", &T::md5_hash
    );
};

struct alignas(16) MemberInfoVo
{
    uint64_t user_id = 0;
    std::string name;
    std::string token;
};
// Glaze meta 注册，写在 MemberInfoVo 定义之后
template <>
struct glz::meta<MemberInfoVo> {
    using T = MemberInfoVo;
    static constexpr auto value = object(
        "user_id", &T::user_id,
        "name", &T::name,
        "token", &T::token
    );
};

// UserDataItem 结构体
struct alignas(16) UserDataItem {
    std::int64_t id{};
    std::string author;
    std::string job_desc;

    void fromRow(const orm::Row& row) {
        id = row["id"].as<std::int64_t>();
        author = row["author"].as<std::string>();
        job_desc = row["job_desc"].as<std::string>();
        // ... 只写一次，复用无限次
    }

    static UserDataItem from(const orm::Row& row) {
        UserDataItem item;
        item.fromRow(row);
        return item;
    }
};

template <>
struct glz::meta<UserDataItem> {
    using T = UserDataItem;
    static constexpr auto value = object(
        "id", &T::id,
        "author", &T::author,
        "job_desc", &T::job_desc
    );
};

// UserDataListVo 结构体
struct alignas(16) UserDataListVo {
    std::int64_t num_users{};
    MemberInfoVo redis_value;
    std::vector<UserDataItem> list; // 用于存储结果
    std::unordered_map<long long, UserDataItem> user_map; // 用于存储结果
};

template <>
struct glz::meta<UserDataListVo> {
    using T = UserDataListVo;
    static constexpr auto value = object(
        "num_users", &T::num_users,
        "redis_value", &T::redis_value,
        "list", &T::list,
        "user_map", &T::user_map
    );
};

// 测试结构体示例
struct MyStruct {
    int id = 1;
    std::string name = "hello glaze";
    std::string message = "性能测试";
};

template <>
struct glz::meta<MyStruct> {
    using T = MyStruct;
    static constexpr auto value = glz::object(
        "id", &T::id,
        "name", &T::name,
        "message", &T::message
    );
};
#endif //DATA_VO_H
