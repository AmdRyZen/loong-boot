// Kafka 落库信封（loong::chat::PersistVo / buildPersistJson / directPartitionKey）单元测试。
//
// 为什么单独一个目标：自带 int main()，且【不依赖 drogon / 网络 / Kafka】——
// 纯逻辑，秒级跑完。
//
// 为什么这些断言值得存在（每条都对应一种「静默失效」）：
//   · 信封键集合是【回放端的 schema 契约】。少一个键 / 改一个名，回放端按固定
//     schema 读就会缺字段 —— 而且不报错，只是字段全空。改之前只有「真实例 +
//     连真 broker + 消费主题」才能发现（e2e 级，还得正好去查）。
//   · 空串字段到底出不出现，取决于 glaze 的 skip_null_members。它默认只跳真正的
//     null、不跳空串（已实测），但这是一条【别人库里的默认值】。哪天升级 glaze
//     或有人给 opts 加了开关，`"toUser":""` 会突然消失 —— 回放端读不到键，
//     同样是静默失败。
//   · 私聊分区键必须【排序后】拼接。忘了排序 ⇒ A→B 与 B→A 落不同分区 ⇒
//     回放时会话内两个方向的先后顺序直接丢失（这正是原实现不传 key 的老问题）。
//
// 不在这里覆盖的：落库开关关闭时 persistXxx 提前返回（省掉信封构造）。那是
// ChatWebsocket 的 private static，需要活实例 + 真 broker 才能观察，已由
// e2e 专项（Kafka ON 路径实测信封内容 + 指标全 0）覆盖。
#include "../utils/ChatPersistEnvelope.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>

using loong::chat::buildPersistJson;
using loong::chat::directPartitionKey;
using loong::chat::nowMs;
using loong::chat::PersistVo;

static int g_failures = 0;

#define CHECK(cond, msg)                       \
    do                                         \
    {                                          \
        if (!(cond))                           \
        {                                      \
            std::printf("  [FAIL] %s\n", msg); \
            ++g_failures;                      \
        }                                      \
        else                                   \
        {                                      \
            std::printf("  [ ok ] %s\n", msg); \
        }                                      \
    } while (0)

// 回放端 schema 契约：11 个键，顺序即 JSON 输出顺序（有些消费端按位置读）。
static constexpr std::array<std::string_view, 11> kExpectedKeys{
    "id", "name", "message", "room", "toUser", "type", "roomSeq", "sender",
    "clientMsgId", "originInstance", "ts"};

// 1. 键集合锁死
static void testEnvelopeKeySet()
{
    std::printf("test: 信封键集合（回放端 schema 契约）\n");

    constexpr auto keys = glz::reflect<PersistVo>::keys;
    CHECK(keys.size() == kExpectedKeys.size(), "键数量 == 11");

    bool sameOrder = keys.size() == kExpectedKeys.size();
    for (size_t i = 0; i < keys.size() && i < kExpectedKeys.size(); ++i)
    {
        if (keys[i] != kExpectedKeys[i])
        {
            sameOrder = false;
            std::printf("  第 %zu 个键: 实际 '%s' / 期望 '%s'\n", i, std::string(keys[i]).c_str(),
                        std::string(kExpectedKeys[i]).c_str());
        }
    }
    CHECK(sameOrder, "键名与声明顺序逐个相符");

    // 序列化结果里每个键都真的出现（反射有 ≠ 输出有：glaze 可能因 opts 跳过）
    PersistVo vo{};
    vo.id = 12345;
    const std::string json = buildPersistJson(vo);
    bool allPresent = true;
    for (const auto k : kExpectedKeys)
    {
        if (json.find("\"" + std::string(k) + "\":") == std::string::npos)
        {
            allPresent = false;
            std::printf("  序列化结果里缺少键: %s\n", std::string(k).c_str());
        }
    }
    CHECK(allPresent, "11 个键全部出现在序列化结果中");
}

// 2. 空串字段照常输出（锁住 glaze skip_null_members 的实际行为）
static void testEmptyStringsAreEmitted()
{
    std::printf("test: 空串字段仍然输出\n");

    // 房间消息：toUser 为空
    PersistVo room{};
    room.room = "r1";
    room.type = "room";
    const std::string roomJson = buildPersistJson(room);
    CHECK(roomJson.find("\"toUser\":\"\"") != std::string::npos,
          "房间消息仍输出 \"toUser\":\"\"");
    CHECK(roomJson.find("\"clientMsgId\":\"\"") != std::string::npos,
          "房间消息仍输出 \"clientMsgId\":\"\"");

    // 私聊：room 为空
    PersistVo direct{};
    direct.toUser = "bob";
    direct.type = "direct";
    const std::string directJson = buildPersistJson(direct);
    CHECK(directJson.find("\"room\":\"\"") != std::string::npos,
          "私聊仍输出 \"room\":\"\"");
}

// 3. 写出去再读回来必须等价（本端写 / 本端读，默认 opts）
static void testRoundTrip()
{
    std::printf("test: 序列化 round-trip\n");

    PersistVo src{};
    src.id = 9007199254740993ULL; // > 2^53：验证 uint64 不被当 double 处理
    src.name = "房间名-中文";
    src.message = "[私聊] hello \"quoted\" \\ slash";
    src.room = "room-a";
    src.toUser = "bob";
    src.type = "direct";
    src.roomSeq = 42;
    src.sender = "alice";
    src.clientMsgId = "c-1";
    src.originInstance = "inst_123_456";
    src.ts = 1757000000123LL;

    const std::string json = buildPersistJson(src);

    PersistVo back{};
    const auto ec = glz::read_json(back, json);
    CHECK(!ec, "反序列化成功");

    CHECK(back.id == src.id, "id 原样（2^53+1 未被浮点化）");
    CHECK(back.name == src.name, "name 原样（含中文）");
    CHECK(back.message == src.message, "message 原样（含转义字符）");
    CHECK(back.room == src.room, "room 原样");
    CHECK(back.toUser == src.toUser, "toUser 原样");
    CHECK(back.type == src.type, "type 原样");
    CHECK(back.roomSeq == src.roomSeq, "roomSeq 原样");
    CHECK(back.sender == src.sender, "sender 原样");
    CHECK(back.clientMsgId == src.clientMsgId, "clientMsgId 原样");
    CHECK(back.originInstance == src.originInstance, "originInstance 原样");
    CHECK(back.ts == src.ts, "ts 原样");
}

// 4. 多一个未知键：默认 opts 整包失败，放宽后能读
//
// 这条锁住的是一个【运维事实】而不是代码行为：glaze 的 error_on_unknown_keys
// 默认 true ⇒ 新实例写了新字段，老实例按默认 opts 读会整条丢弃。所以回放端
// 必须显式用 error_on_unknown_keys = false。写在这里是防止有人「顺手改回默认」。
static void testUnknownKeyTolerance()
{
    std::printf("test: 未知键的读写容错（新老实例混跑）\n");

    PersistVo vo{};
    vo.id = 7;
    vo.type = "room";
    std::string json = buildPersistJson(vo);

    // 模拟「未来版本多写了一个字段」
    const std::string withExtra = "{\"futureField\":\"x\"," + json.substr(1);
    CHECK(withExtra.size() == json.size() + std::string("{\"futureField\":\"x\",").size() - 1,
          "构造出的 JSON 只多了一个未知键");

    PersistVo strictVo{};
    const auto strictEc = glz::read_json(strictVo, withExtra);
    CHECK(static_cast<bool>(strictEc), "默认 opts 遇到未知键会失败（glaze 默认行为）");

    PersistVo lenientVo{};
    const auto lenientEc =
        glz::read<glz::opts{.error_on_unknown_keys = false}>(lenientVo, withExtra);
    CHECK(!lenientEc, "error_on_unknown_keys=false 时能读成功");
    CHECK(lenientVo.id == 7 && lenientVo.type == "room", "放宽后字段值仍然正确");
}

// 5. 私聊分区键：两方向同键
static void testDirectPartitionKey()
{
    std::printf("test: 私聊分区键\n");

    const std::string ab = directPartitionKey("alice", "bob");
    const std::string ba = directPartitionKey("bob", "alice");
    CHECK(ab == "alice|bob", "键就是排序后拼接 alice|bob");
    CHECK(ab == ba, "A→B 与 B→A 得到同一个键（落同一分区，会话内保序）");

    CHECK(directPartitionKey("bob", "bob") == "bob|bob", "自己给自己发不崩、键稳定");
    CHECK(directPartitionKey("", "bob") == "|bob", "空昵称有确定行为");
    CHECK(directPartitionKey("", "") == "|", "双空昵称有确定行为");

    // 前缀/长度边界：不能被误判成相等
    CHECK(directPartitionKey("a", "ab") != directPartitionKey("a", "a"),
          "a|ab 与 a|a 不同键");
    CHECK(directPartitionKey("a", "ab") == directPartitionKey("ab", "a"),
          "a|ab 两方向同键");

    // 大小写敏感：'A' < 'a'，排序必须按字节序而非忽略大小写
    CHECK(directPartitionKey("Bob", "alice") == "Bob|alice",
          "按字节序排序（大写在前），与 locale 无关");

    // 稳定性：同一对昵称反复调用结果一致（不能依赖容器迭代顺序之类）
    CHECK(directPartitionKey("zeta", "alpha") == directPartitionKey("zeta", "alpha"),
          "同一输入重复调用结果一致");
}

// 6. nowMs 合理性
static void testNowMs()
{
    std::printf("test: nowMs\n");

    // 2024-01-01T00:00:00Z 的毫秒数。低于它说明用了错误的时钟/单位。
    constexpr int64_t kEpoch2024Ms = 1704067200000LL;
    const int64_t a = nowMs();
    CHECK(a > kEpoch2024Ms, "大于 2024-01-01（单位确实是毫秒）");

    const auto sysNow = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    CHECK(a <= sysNow + 1, "不超前于系统时钟（不是单调时钟/不是纳秒）");

    const int64_t b = nowMs();
    CHECK(b >= a, "单调不减");
}

int main()
{
    std::printf("=== Kafka 落库信封 单元测试 ===\n\n");
    testEnvelopeKeySet();
    std::printf("\n");
    testEmptyStringsAreEmitted();
    std::printf("\n");
    testRoundTrip();
    std::printf("\n");
    testUnknownKeyTolerance();
    std::printf("\n");
    testDirectPartitionKey();
    std::printf("\n");
    testNowMs();
    std::printf("\n");

    if (g_failures == 0)
    {
        std::printf("ALL PASSED (failures=0)\n");
        return 0;
    }
    std::printf("FAILED (failures=%d)\n", g_failures);
    return 1;
}
