//
// Created by 神圣·凯莎 on 2022/4/5.
//

extern "C" {
#include "aes.h"
#include "base64Utils.h"
}
#include "cipherUtils.h"

//get the bit length after zero padding (one of 128, 192, 256), skey.length() must be <= 32.
int keybits(const std::string& skey)
{
    int len = skey.length() < 16 ? 16 : skey.length();  //make sure (len/8)*64 >= 128
    return (len / 8) * 64;
}

//a向上取整为b的倍数
inline int roundon(int a, int b)
{
    return a % b == 0 ? a : a + b - a % b;
}

//copy and do Pkcs7 padding, src needs to be null-terminated, buff needs to be big enough.
int padcpy(uint8_t* buff, const void* src)
{
    auto srclen = strlen((const char*)src);
    memcpy(buff, src, srclen);

    uint8_t padval = BLOCK_SIZE - srclen % BLOCK_SIZE;  //0 < padval <= 16
    for (size_t i = srclen; i < srclen + padval; i++)
        buff[i] = padval;
    return srclen + padval;
}

inline void XOR(const uint8 a[16], const uint8 b[16], uint8 c[16])
{
    for (int i = 0; i < 16; i++)
        c[i] = a[i] ^ b[i];
}

//note: the length of skey should be <= 32, the length of siv should be <= 16, no length check conducted
std::string cipherUtils::encrypt_cbc(const std::string& plaintext, const std::string& skey, const std::string& siv)
{
    uint8 iv[16] = {0};  //zero padding
    memcpy(iv, siv.c_str(), siv.length());
    aes_context ctx;
    uint8 key[32] = {0};  //zero padding
    memcpy(key, skey.c_str(), skey.length());
    aes_set_key(&ctx, key, keybits(skey));

    size_t outlen = (plaintext.length() / BLOCK_SIZE + 1) * BLOCK_SIZE;  //padding之后plaintext的长度
    uint8* output = new uint8[outlen];
    padcpy(output, plaintext.c_str());

    //每次处理16字节
    for (size_t i = 0; i < outlen; i += BLOCK_SIZE)
    {
        XOR(i == 0 ? iv : output + i - BLOCK_SIZE, output + i, output + i);  //当前明文组先与上一密文组异或处理
        aes_encrypt(&ctx, output + i, output + i);                           //note that input and output could use the same buffer
    }
    size_t baselen = roundon(outlen, 3) / 3 * 4;
    char* basebuf = new char[baselen];
    base64_encode(output, outlen, basebuf);
    std::string ret(basebuf, baselen);
    delete[] basebuf;
    delete[] output;
    return ret;
}

std::string cipherUtils::decrypt_cbc(const std::string& basestr, const std::string& skey, const std::string& siv)
{
    uint8 iv[16] = {0};  //zero padding
    memcpy(iv, siv.c_str(), siv.length());
    aes_context ctx;
    uint8 key[32] = {0};
    memcpy(key, skey.c_str(), skey.length());
    aes_set_key(&ctx, key, keybits(skey));

    size_t outlen = basestr.length() / 4 * 3;
    uint8* output = new uint8[outlen];  //because of the trailing '='s, there may be one or two extra bytes, no biggie
    outlen = base64_decode(basestr.c_str(), basestr.length(), output);

    //每次处理16字节
    uint8 lastblock[16];
    uint8 currblock[16];
    for (size_t i = 0; i < outlen; i += BLOCK_SIZE)
    {
        memcpy(currblock, output + i, 16);  //当前密文组要保留以供下一轮使用
        aes_decrypt(&ctx, output + i, output + i);
        XOR(i == 0 ? iv : lastblock, output + i, output + i);  //解密完还要与上一密文组异或

        memcpy(lastblock, currblock, 16);
    }

    output[outlen - output[outlen - 1]] = '\0';  //unpad
    std::string ret = (char*)output;
    delete[] output;
    return ret;
}
