//
// Created by 天使之王·彦 on 2021/11/13.
//

#include "../TrieService.h"
#include "fstream"
#include "iostream"
#include "../SbcConvertService.h"
#include <ranges>

constexpr wchar_t kEndFlag = L'ﾰ';  // unicode: FFB0

static bool isStopCharacter(const wchar_t ch)
{
    const int code = static_cast<int>(static_cast<unsigned char>(ch));
    // 常见全角标点和符号区间
    if (code >= 0x3000 && code <= 0x303F)
        return true;
    // 常见符号区间
    if (code >= 0x2000 && code <= 0x206F)
        return true;
    // Emoji 区间
    if (code >= 0x1F300 && code <= 0x1FAFF)
        return true;
    return false;
}

TrieNode::TrieNode() = default;

TrieNode::~TrieNode()
{
    // 使用范围for循环和views::values删除subNodes_中的值
    for (const auto* value : std::ranges::subrange(subNodes_ | std::views::values))
    {
        delete value;
    }
    subNodes_.clear();
}

TrieService::TrieService()
{
    root_ = new TrieNode();
}

TrieService::~TrieService()
{
    delete root_;
    root_ = nullptr;
}

void TrieService::insert(const std::wstring& word)
{
    TrieNode* curNode = root_;
    for (wchar_t code : word)
    {
        const int unicode = SbcConvertService::charConvert(code);
        TrieNode* subNode = curNode->getSubNode(unicode);

        if (subNode == nullptr)
        {
            subNode = new TrieNode();
            curNode->addSubNode(unicode, subNode);
        }
        curNode = subNode;
    }
    const int unicode = SbcConvertService::charConvert(kEndFlag);
    curNode->addSubNode(unicode, new TrieNode());
}

bool TrieService::search(const std::wstring& word)
{
    bool is_contain = false;
    for (size_t i = 0; i < word.length(); ++i)
    {
        if (const int wordLen = getSensitiveLength(word, i); wordLen > 0)
        {
            is_contain = true;
            break;
        }
    }
    return is_contain;
}

bool TrieService::startsWith(const std::wstring& prefix)
{
    TrieNode* curNode = root_;
    for (wchar_t item : prefix)
    {
        const int unicode = SbcConvertService::charConvert(item);
        curNode = curNode->getSubNode(unicode);
        if (curNode == nullptr)
        {
            return false;
        }
    }
    return true;
}

std::set<SensitiveWord> TrieService::getSensitive(const std::wstring& word)
{
    std::set<SensitiveWord> sensitiveSet;
    for (size_t i = 0; i < word.length(); ++i)
    {
        if (const int wordLen = getSensitiveLength(word, i); wordLen > 0)
        {
            const std::wstring sensitiveWord = word.substr(i, wordLen);
            SensitiveWord wordObj;
            wordObj.word = sensitiveWord;
            wordObj.startIndex = i;
            wordObj.len = wordLen;

            sensitiveSet.insert(wordObj);
            i = i + wordLen - 1;
        }
    }
    return sensitiveSet;
}

int TrieService::getSensitiveLength(const std::wstring& word, const size_t startIndex)
{
    TrieNode* p1 = root_;
    int wordLen = 0;
    bool endFlag = false;
    for (size_t p3 = startIndex; p3 < word.length(); ++p3)
    {
        const int unicode = SbcConvertService::charConvert(word[p3]);
        const auto subNode = p1->getSubNode(unicode);
        if (subNode == nullptr)
        {
            if (stop_words_.contains(unicode) || isStopCharacter(word[p3]))
            {
                ++wordLen;
                continue;
            }
            break;
        }
        {
            ++wordLen;
            // 直到找到尾巴的位置，才认为完整包含敏感词
            if (subNode->getSubNode(SbcConvertService::charConvert(kEndFlag)))
            {
                endFlag = true;
                break;
            }
            {
                p1 = subNode;
            }
        }
    }
    // 注意，处理一下没找到尾巴的情况
    if (!endFlag)
    {
        wordLen = 0;
    }
    return wordLen;
}

std::wstring TrieService::replaceSensitive(const std::wstring& word)
{
    std::set<SensitiveWord> words = getSensitive(word);
    std::wstring ret = word;
    for (const auto& [word, startIndex, len] : words)
    {
        for (size_t i = startIndex; i < startIndex + len; ++i)
        {
            ret[i] = L''; // 确保编译器和环境正确处理宽字符
        }
    }
    return ret;
}

void TrieService::loadFromFile(const std::string& file_name)
{
    std::ifstream ifs(file_name, std::ios_base::in);
    std::string str;
    int count = 0;
    while (getline(ifs, str))
    {
        std::wstring utf8_str = SbcConvertService::s2ws(str);
        insert(utf8_str);
        count++;
    }
    //std::cout << "load " << count << " words" << std::endl;
}

void TrieService::loadStopWordFromFile(const std::string& file_name)
{
    std::ifstream ifs(file_name, std::ios_base::in);
    std::string str;
    int count = 0;
    while (getline(ifs, str))
    {
        std::wstring utf8_str = SbcConvertService::s2ws(str);
        if (utf8_str.length() == 1)
        {
            stop_words_.insert(utf8_str[0]);
            count++;
        }
        else if (utf8_str.empty())
        {
            stop_words_.insert(L' ');
            count++;
        }
    }
    //std::cout << "load " << count << " stop words" << std::endl;
}

void TrieService::loadStopWordFromMemory(const std::unordered_set<wchar_t>& words)
{
    for (auto& str : words)
    {
        int unicode = SbcConvertService::charConvert(str);
        stop_words_.emplace(unicode);
    }
}

void TrieService::loadFromMemory(const std::unordered_set<std::wstring>& words)
{
    for (auto& item : words)
    {
        insert(item);
    }
}
