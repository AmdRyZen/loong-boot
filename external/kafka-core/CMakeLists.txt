cmake_minimum_required(VERSION 3.10)  # 降低版本要求以提高兼容性
# 编译器路径原先硬编码为 Homebrew gcc 15.1.0。该版本被 brew 升级移除后，
# build-lib.sh 直接 configure 失败（"is not a full path to an existing
# compiler tool"）—— 于是 kafka-core 的源码改动根本无法重装，
# 表现为「改了 src/ 却怎么都不生效」。
# 这里改成「存在则显式指定，不存在则回退 PATH 上的默认编译器」。
if(EXISTS "/opt/homebrew/bin/gcc-16" AND EXISTS "/opt/homebrew/bin/g++-16")
    set(CMAKE_C_COMPILER "/opt/homebrew/bin/gcc-16")
    set(CMAKE_CXX_COMPILER "/opt/homebrew/bin/g++-16")
endif()

project(kafka-core)

set(CMAKE_CXX_STANDARD 26)

add_library(kafka-core STATIC
        src/KafkaManager.cpp
)

target_include_directories(kafka-core PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# 使用 FetchContent 拉取 librdkafka
include(FetchContent)
FetchContent_Declare(
        librdkafka
        GIT_REPOSITORY https://github.com/confluentinc/librdkafka.git
        GIT_TAG v2.10.0
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(librdkafka)

# 链接 librdkafka
target_link_libraries(kafka-core PUBLIC rdkafka)

# 安装 kafka-core
install(TARGETS kafka-core
        EXPORT KafkaCoreTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)

# 安装头文件
install(DIRECTORY include/ DESTINATION include)

# 安装 librdkafka
install(TARGETS rdkafka
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)
install(DIRECTORY ${librdkafka_SOURCE_DIR}/src/ DESTINATION include
        FILES_MATCHING PATTERN "*.h")

# 安装 CMake 配置文件
install(EXPORT KafkaCoreTargets
        FILE KafkaCoreTargets.cmake
        NAMESPACE Kafka::
        DESTINATION lib/cmake/KafkaCore)

include(CMakePackageConfigHelpers)

write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/KafkaCoreConfigVersion.cmake"
        VERSION 1.0.0  # 替换为你项目的版本
        COMPATIBILITY SameMajorVersion
)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/KafkaCoreConfigVersion.cmake"
        DESTINATION lib/cmake/KafkaCore
)