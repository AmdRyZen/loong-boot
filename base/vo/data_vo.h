//
// Created by 神圣•凯莎 on 24-8-2.
//

#ifndef DATA_VO_H
#define DATA_VO_H

#include <cstdint>
#include <string>
#include <vector>

// UserDataItem 结构体
struct alignas(16) UserDataItem {
    std::int64_t id{};
    std::string author;
    std::string job_desc;
};

// UserDataListVo 结构体
struct alignas(16) UserDataListVo {
    std::int64_t num_users{};
    std::string redis_value;
    std::vector<UserDataItem> list; // 用于存储结果
};

// 定义 AesResponseDataVo 结构体
struct alignas(16) AesResponseDataVo {
    std::string encrypted;
    std::string decrypted;
    std::string hash;
    std::string md5_hash;
};


struct alignas(16) MemberInfoVo
{
    uint64_t user_id;
    std::string name;
    std::string token;
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
