//
// Created by 神圣•凯莎 on 24-8-2.
//

#ifndef DATA_VO_H
#define DATA_VO_H

#include <string>
#include <tbb/concurrent_vector.h>

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

#include <vector>
#include <map>
#include <array>

struct NestedStruct {
    int nested_id = 42;
    std::string nested_name = "nested";
};

template <>
struct glz::meta<NestedStruct> {
    using T = NestedStruct;
    static constexpr auto value = object(
        "nested_id", &T::nested_id,
        "nested_name", &T::nested_name
    );
};

struct alignas(16) MyStruct
{
    int id = 1;
    double d = 3.14;
    bool active = true;
    std::string name = "Hello, this is a glaze response";
    std::string message = "我草";

    std::array<uint64_t, 3> arr = { 1, 2, 3 };
    std::vector<int> vec = {10, 20, 30};
    std::map<std::string, int> map{{"one", 1}, {"two", 2}};

    NestedStruct nested{};
};

template <>
struct glz::meta<MyStruct> {
    using T = MyStruct;
    static constexpr auto value = object(
        "id", &T::id,
        "d", &T::d,
        "active", &T::active,
        "name", &T::name,
        "message", &T::message,
        "arr", &T::arr,
        "vec", &T::vec,
        "map", &T::map,
        "nested", &T::nested
    );
};

#endif //DATA_VO_H
