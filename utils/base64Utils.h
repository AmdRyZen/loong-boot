//
// Created by 神圣·凯莎 on 2022/4/5.
//

#ifndef DROGON_HTTP_BASE64UTILS_H
#define DROGON_HTTP_BASE64UTILS_H

int base64_encode(const unsigned char* src, int src_bytes, char* out);

int base64_decode(const char* in, int in_bytes, unsigned char* out);

#endif  //DROGON_HTTP_BASE64UTILS_H
