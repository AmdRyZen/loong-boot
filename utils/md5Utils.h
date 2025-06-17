//
// Created by 神圣·凯莎 on 2022/4/6.
//

#ifndef DROGON_HTTP_MD5UTILS_H
#define DROGON_HTTP_MD5UTILS_H

typedef struct
{
    unsigned int count[2];
    unsigned int state[4];
    unsigned char buffer[64];
} MD5_CTX;

#define F1(x, y, z) ((x & y) | (~x & z))
#define G(x, y, z) ((x & z) | (y & ~z))
#define H(x, y, z) (x ^ y ^ z)
#define I(x, y, z) (y ^ (x | ~z))
#define ROTATE_LEFT(x, n) ((x << n) | (x >> (32 - n)))
#define FF(a, b, c, d, x, s, ac)   \
    {                              \
        a += F1(b, c, d) + x + ac; \
        a = ROTATE_LEFT(a, s);     \
        a += b;                    \
    }
#define GG(a, b, c, d, x, s, ac)  \
    {                             \
        a += G(b, c, d) + x + ac; \
        a = ROTATE_LEFT(a, s);    \
        a += b;                   \
    }
#define HH(a, b, c, d, x, s, ac)  \
    {                             \
        a += H(b, c, d) + x + ac; \
        a = ROTATE_LEFT(a, s);    \
        a += b;                   \
    }
#define II(a, b, c, d, x, s, ac)  \
    {                             \
        a += I(b, c, d) + x + ac; \
        a = ROTATE_LEFT(a, s);    \
        a += b;                   \
    }

class md5Utils
{
  public:
    static void MD5Init(MD5_CTX* context);
    static void MD5Update(MD5_CTX* context, unsigned char* input, unsigned int inputlen);
    static void MD5Final(MD5_CTX* context, unsigned char digest[16]);
    static void MD5Transform(unsigned int state[4], unsigned char block[64]);
    static void MD5Encode(unsigned char* output, const unsigned int* input, unsigned int len);
    static void MD5Decode(unsigned int* output, const unsigned char* input, unsigned int len);
};

#endif  //DROGON_HTTP_MD5UTILS_H
