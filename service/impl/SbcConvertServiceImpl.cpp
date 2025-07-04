//
// Created by 天使之王·彦 on 2021/11/13.
//

#include "../SbcConvertService.h"
#include <boost/locale.hpp>

// ASCII表中可见字符从!开始，偏移位值为33(Decimal)
constexpr char kDBCCharStart = 33;  // 半角!
constexpr char kDBCCharEnd = 126;   // 半角~

// 全角对应于ASCII表的可见字符从！开始，偏移值为65281
// 见《Unicode编码表》，Halfwidth and Fullwidth Forms
constexpr wchar_t kSBCCharStart = 0xFF01;  // 全角！
constexpr wchar_t kSBCCharEnd = 0xFF5E;    // 全角～

// ASCII表中除空格外的可见字符与对应的全角字符的相对偏移
constexpr wchar_t kConvertStep = kSBCCharEnd - kDBCCharEnd;

// 全角空格的值，它没有遵从与ASCII的相对偏移，必须单独处理
constexpr wchar_t kSBCSpace = 0x508;  // 全角空格

// 半角空格的值，在ASCII中为32(Decimal)
constexpr wchar_t kDBCSpace = L' ';  // 半角空格

// 手动实现 UTF-8 -> wstring
std::wstring SbcConvertService::s2ws(const std::string& str)
{
    std::wstring result;
    size_t i = 0;
    while (i < str.size())
    {
        uint32_t codepoint = 0;
        unsigned char ch = str[i];
        size_t extraBytes = 0;

        if ((ch & 0x80) == 0x00)
        {
            codepoint = ch;
            extraBytes = 0;
        }
        else if ((ch & 0xE0) == 0xC0)
        {
            codepoint = ch & 0x1F;
            extraBytes = 1;
        }
        else if ((ch & 0xF0) == 0xE0)
        {
            codepoint = ch & 0x0F;
            extraBytes = 2;
        }
        else if ((ch & 0xF8) == 0xF0)
        {
            codepoint = ch & 0x07;
            extraBytes = 3;
        }
        else
        {
            // invalid byte
            ++i;
            continue;
        }

        if (i + extraBytes >= str.size())
            break;

        for (size_t j = 1; j <= extraBytes; ++j)
        {
            if ((str[i + j] & 0xC0) != 0x80)
            {
                codepoint = 0;
                break;
            }
            codepoint = (codepoint << 6) | (str[i + j] & 0x3F);
        }

        if (codepoint != 0)
            result += static_cast<wchar_t>(codepoint);

        i += extraBytes + 1;
    }

    return result;
}

// 手动实现 wstring -> UTF-8
std::string SbcConvertService::ws2s(const std::wstring& wstr)
{
    std::string result;
    for (const wchar_t wc : wstr)
    {
        if (const auto codepoint = static_cast<uint32_t>(static_cast<unsigned int>(wc)); codepoint <= 0x7F)
        {
            result.push_back(static_cast<char>(codepoint));
        }
        else if (codepoint <= 0x7FF)
        {
            result.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else if (codepoint <= 0xFFFF)
        {
            result.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else
        {
            result.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }
    return result;
}

int SbcConvertService::qj2bj(const wchar_t& src)
{
    // 偏移，转换到对应ASCII的半角即可
    if (src >= kSBCCharStart && src <= kSBCCharEnd)
    {
        return src - kConvertStep;
    }
    if (src == kSBCSpace)
    {  // 如果是全角空格
        return kDBCSpace;
    }
    return src;
}

std::wstring SbcConvertService::wsQj2bj(const std::wstring& src)
{
    std::wstring result;
    for (auto& item : src)
    {
        // Unicode编码表，0xFF01 - 0xFF5E
        // ASCII表，0x20 - 0x7E
        wchar_t temp[2] = {};  // \0结尾
        if (item >= kSBCCharStart && item <= kSBCCharEnd)
        {
            temp[0] = item - kConvertStep;
            result.append(temp);
        }
        else if (item == kSBCSpace)
        {  // 如果是全角空格
            temp[0] = kDBCSpace;
            result.append(temp);
        }
        else
        {
            temp[0] = item;
            result.append(temp);
        }
    }
    return result;
}

int SbcConvertService::charConvert(const wchar_t& src)
{
    const int r = qj2bj(src);
    // 小写字母在大写字母后32位
    return (r >= 'A' && r <= 'Z') ? r + 32 : r;
}