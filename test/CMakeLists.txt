cmake_minimum_required(VERSION 3.28)
project(loong-boot_test CXX)

add_executable(${PROJECT_NAME} test_main.cc)

# ##############################################################################
# If you include the drogon source code locally in your project, use this method
# to add drogon 
# target_link_libraries(${PROJECT_NAME}_test PRIVATE drogon)
#
# and comment out the following lines
target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon)

ParseAndAddDrogonTests(${PROJECT_NAME})

# ##############################################################################
# RoomRegistry 的独立单元测试
#
# ⚠️ test_room_registry.cc 自带 int main()（自己实现 CHECK 计数，不走 drogon
#    测试框架），因此必须单独成一个可执行目标，不能并进 test_main.cc
#    —— 否则会 main 重复定义。
#
#    在此之前它没有被任何目标编译过：保序 / 背压 / reshape / 按 loop 分组投递
#    这些断言实际上从未被执行，任何回归都不会被发现。
# ##############################################################################
add_executable(${PROJECT_NAME}_room_registry test_room_registry.cc)
target_link_libraries(${PROJECT_NAME}_room_registry PRIVATE Drogon::Drogon)

# ##############################################################################
# SnowflakeId（全局唯一消息 ID）单元测试
#
# 同样自带 int main()，且【不依赖 drogon】—— 纯逻辑，只 include 头文件。
# 之所以单独一个目标而不是并进 room_registry：它验证的是 ID 生成器，
# 与房间注册表无关，混在一起会让「哪个失败」变得难判读。
# ##############################################################################
add_executable(${PROJECT_NAME}_snowflake_id test_snowflake_id.cc)

# ##############################################################################
# Kafka 落库信封（utils/ChatPersistEnvelope.h）单元测试
#
# 自带 int main()，同样【不依赖 drogon / 网络 / Kafka】—— 只 include 头文件，
# 唯一的外部依赖是 glaze（纯头文件库，链接 glaze::glaze 只为拿包含目录）。
#
# 覆盖的是三件「错了也不报错、只静默失效」的事：
#   ① 信封键集合（回放端 schema 契约，少一个键就静默缺字段）
#   ② 空串字段是否照常输出（glaze skip_null_members 的默认行为，属别人的默认值）
#   ③ 私聊分区键必须排序后拼接（否则 A→B / B→A 散在不同分区，会话内顺序丢失）
# 这三条以前只能靠「真实例 + 真 broker + 消费主题」验证（e2e 级）。
# ##############################################################################
add_executable(${PROJECT_NAME}_chat_persist test_chat_persist.cc)
target_link_libraries(${PROJECT_NAME}_chat_persist PRIVATE glaze::glaze)

enable_testing()
add_test(NAME room_registry COMMAND ${PROJECT_NAME}_room_registry)
add_test(NAME snowflake_id COMMAND ${PROJECT_NAME}_snowflake_id)
add_test(NAME chat_persist COMMAND ${PROJECT_NAME}_chat_persist)
