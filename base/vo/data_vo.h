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
    static constexpr auto value = glz::object(
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

// UserDataListVo 结构体
struct alignas(16) UserDataListVo {
    std::int64_t num_users{};
    MemberInfoVo redis_value;
    std::vector<UserDataItem> list; // 用于存储结果
    std::unordered_map<long long, UserDataItem> user_map; // 用于存储结果
};

struct alignas(16) MyStruct
{
    int id = 1;
    /*double d = 3.14;*/
    std::string name = "Hello, this is a glaze response";
    std::string message = "我草";
    /*std::array<uint64_t, 3> arr = { 1, 2, 3 };
    std::map<std::string, int> map{{"one", 1}, {"two", 2}};*/
};

#endif //DATA_VO_H
