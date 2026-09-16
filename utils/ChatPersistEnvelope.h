//
// 聊天消息的 Kafka 落库信封 —— 与 ChatWebsocket 解耦，便于单测。
//
// 为什么单独抽出来：这几个东西原本是 ChatWebsocket 的 private static 成员，
// 只有「跑一遍真实例 + 真 broker + 消费主题」才能验证（e2e 级），
// 而它们恰恰是最容易写错又最难发现的一类逻辑：
//   · 信封少一个键 ⇒ 回放端按固定 schema 读会缺字段；
//   · 私聊分区键忘了排序 ⇒ A→B 与 B→A 落不同分区，会话内顺序静默丢失。
// 抽成纯函数后就能用普通单测锁死（见 test/test_chat_persist.cc）。
//

#ifndef CHAT_PERSIST_ENVELOPE_H
#define CHAT_PERSIST_ENVELOPE_H

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include <glaze/glaze.hpp>

namespace loong::chat
{
    // 落库信封：比客户端 VO（chatMessageVo）多带「回放需要的上下文」。
    //
    // ⚠️ 这个结构体的键集合是【回放端的 schema 契约】：
    // 新增字段是安全的（老消费端忽略），但**删字段或改名会让历史数据读不出来**。
    // test_chat_persist.cc 里有一条用例把 11 个键全部锁死（含空串字段）。
    struct PersistVo
    {
        uint64_t id = 0;             // 与客户端看到的 id 一致（snowflake，全局唯一）
        std::string name;            // 与客户端 VO 的 name 一致（公告里它是房间名，历史原因）
        std::string message;         // 与客户端看到的文本完全一致（含 "[私聊] " 前缀）
        std::string room;            // 房间名（房间消息 / 公告）；私聊为空
        std::string toUser;          // 私聊目标昵称；房间消息为空
        std::string type;            // "room" | "direct" | "notice"
        uint64_t roomSeq = 0;        // 房间级序号（房间消息；0 = 未分配）
        std::string sender;          // 真实发送者昵称（公告里 name 是房间名，故单列）
        std::string clientMsgId;     // 客户端 dto 的 key 字段，协议未定义语义，预留做幂等
        std::string originInstance;  // 落库实例 ID：多实例时用于追查「哪个实例写的」
        int64_t ts = 0;              // 落库时刻（Unix 毫秒，UTC）
    };

    inline int64_t nowMs() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // 序列化成一行 JSON。
    //
    // ⚠️ 空字符串字段【会】照常输出（实测 `"toUser":""` / `"clientMsgId":""` 都在）。
    // glaze 的 `skip_null_members`（默认 true）只跳过真正的 null，不跳空串 ——
    // 但这条依赖值得用测试锁住：万一将来有人给 opts 里加了别的开关，
    // 回放端会突然读不到键，而那种失败是静默的。
    inline std::string buildPersistJson(const PersistVo& vo)
    {
        std::string json{};
        (void)glz::write_json(vo, json);
        return json;
    }

    // 私聊的分区键 = 排序后拼接的双方昵称。
    //
    // 必须是【排序后】拼接，不能按 (发送者, 接收者) 顺序：同一个会话的两个方向
    // （A→B 与 B→A）要落同一个分区，否则回放时两个方向散在不同分区，会话内的
    // 先后顺序就丢了 —— 这正是原实现不传 key 时的问题。
    //
    // 分隔符用 '|'：昵称里出现 '|' 会让 `a|b` 与 `a` + `|b` 撞键，属于已知的可接受风险
    // （昵称由客户端 header 提供，本工程未做字符白名单）。
    inline std::string directPartitionKey(std::string_view a, std::string_view b)
    {
        const std::string_view lo = a < b ? a : b;
        const std::string_view hi = a < b ? b : a;
        std::string key;
        key.reserve(lo.size() + hi.size() + 1);
        key.append(lo).push_back('|');
        key.append(hi);
        return key;
    }
} // namespace loong::chat

#endif // CHAT_PERSIST_ENVELOPE_H
