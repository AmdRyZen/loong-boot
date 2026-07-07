//
// Created by 天使之王·彦 on 2021/11/13.
//

#ifndef LEARNING_CPP_TRIESERVICE_H
#define LEARNING_CPP_TRIESERVICE_H

#include "set"
#include "string"
#include "unordered_map"
#include "unordered_set"

class TrieNode
{
  public:
    TrieNode();

    ~TrieNode();

    void addSubNode(const uint16_t c, TrieNode* subNode)
    {
        subNodes_[c] = subNode;
    }

    TrieNode* getSubNode(const uint16_t c) const
    {
        const auto it = subNodes_.find(c);
        return it == subNodes_.end() ? nullptr : it->second;
    }

  private:
    // 因为内部实现了哈希表，因此其查找速度非常的快
    // 缺点： 哈希表的建立比较耗费时间
    std::unordered_map<uint16_t, TrieNode*> subNodes_;
};

struct SensitiveWord
{
    std::wstring word;
    size_t startIndex;
    int len;

    friend bool operator<(struct SensitiveWord const& a, struct SensitiveWord const& b)
    {
        return a.startIndex < b.startIndex;
    }
};

class TrieService
{
  public:
    TrieService();

    ~TrieService();

    // 禁止🈲
    TrieService(const TrieService& that) = delete;

    TrieService& operator=(const TrieService& thad) = delete;

    //从文件加载敏感词列表，文件utf8格式，一个敏感词单独一行
    static void loadFromFile(const std::string& file_name);

    //从内存加载敏感词列表
    [[gnu::always_inline]] inline static void loadFromMemory(const std::unordered_set<std::wstring>& words);

    //加载停顿词从指定的文件
    static void loadStopWordFromFile(const std::string& file_name);

    //从内存加载停顿词
    [[gnu::always_inline]] inline static void loadStopWordFromMemory(const std::unordered_set<wchar_t>& words);

    //brief Inserts a word into the trie
    [[gnu::always_inline]] inline static void insert(const std::wstring& word);

    //brief Returns if the word is in the trie
    [[gnu::always_inline]] inline static bool search(const std::wstring& word);

    //brief Returns if there is any word in the trie that starts with the given prefix
    [[gnu::always_inline]] inline static bool startsWith(const std::wstring& prefix);

    //过滤敏感词并返回敏感词命中位置和信息
    static std::set<SensitiveWord> getSensitive(const std::wstring& word);

    // 替换敏感词为*
    static std::wstring replaceSensitive(const std::wstring& word);

  private:
    [[gnu::always_inline]] inline static int getSensitiveLength(const std::wstring& word, size_t startIndex);

    inline static TrieNode* root_;
    inline static std::unordered_set<uint16_t /*unicode*/> stop_words_;
};

#endif  //LEARNING_CPP_TRIESERVICE_H
