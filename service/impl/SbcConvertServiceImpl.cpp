//
// Created by 天使之王·彦 on 2021/11/13.
//

#include "../SbcConvertService.h"
#include <boost/locale.hpp>
#include "iostream"

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

std::wstring SbcConvertService::s2ws(const std::string& str)
{
    if (str.empty()) return {};

    std::wstring result;
    result.reserve(str.size()); // 估计最大长度

    auto state = std::mbstate_t();
    const char* src = str.data();
    size_t len = str.size();
    wchar_t wc;

    while (len > 0)
    {
        const size_t ret = std::mbrtowc(&wc, src, len, &state);
        if (ret == static_cast<size_t>(-1) || ret == static_cast<size_t>(-2))
            throw std::runtime_error("flowchart conversion failed");

        result.push_back(wc);
        src += ret;
        len -= ret;
    }
    return result;
}

std::string SbcConvertService::ws2s(const std::wstring& wstr)
{
    if (wstr.empty())
        return {};

    const size_t maxSize = wstr.size() * 4;
    std::string result;
    result.reserve(maxSize);

    std::mbstate_t state = std::mbstate_t();
    char string[MB_LEN_MAX];
    for (const wchar_t wc : wstr)
    {
        const size_t len = std::wcrtomb(string, wc, &state);
        if (len == static_cast<size_t>(-1)) {
            std::memset(&state, 0, sizeof(state)); // 重置状态
            continue;
        }
        result.append(string, len);
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
    else if (src == kSBCSpace)
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