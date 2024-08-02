//
// Created by 神圣•凯莎 on 24-8-2.
//

#ifndef DATA_VO_H
#define DATA_VO_H

#include <cstdint>
#include <string>
#include <vector>

// UserDataItem 结构体
struct UserDataItem {
    std::int64_t id{};
    std::string author;
    std::string job_desc;
};

// UserDataListVo 结构体
struct UserDataListVo {
    std::int64_t num_users{};
    std::string redis_value;
    std::vector<UserDataItem> list; // 用于存储结果
};

// 定义 AesResponseDataVo 结构体
struct AesResponseDataVo {
    std::string encrypted;
    std::string decrypted;
    std::string hash;
    std::string md5_hash;
};


struct MemberInfoVo
{
    uint64_t user_id;
    std::string name;
    std::string token;
};

struct MyStruct
{
    int i = 1;
    /*double d = 3.14;*/
    std::string hello = "c";
    /*std::array<uint64_t, 3> arr = { 1, 2, 3 };
    std::map<std::string, int> map{{"one", 1}, {"two", 2}};*/
};

#endif //DATA_VO_H
