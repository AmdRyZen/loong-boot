//
// Created by 神圣·凯莎 on 2022/4/5.
//

#ifndef DROGON_HTTP_CIPHERUTILS_H
#define DROGON_HTTP_CIPHERUTILS_H

#pragma once
#include <string.h>
#include <string>

#define BLOCK_SIZE 16

class cipherUtils
{
  public:
    static std::string encrypt_cbc(const std::string& plaintext, const std::string& skey, const std::string& siv);

    static std::string decrypt_cbc(const std::string& basestr, const std::string& skey, const std::string& siv);
};

#endif  //DROGON_HTTP_CIPHERUTILS_H
