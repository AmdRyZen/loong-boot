cmake_minimum_required(VERSION 3.31.6)

# 检查 CMake 版本，设置政策
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
else()
    message(STATUS "CMake version does not support policy CMP0167")
endif()

# 设置编译器
set(CMAKE_C_COMPILER "/opt/homebrew/Cellar/gcc/15.1.0/bin/gcc-15")
set(CMAKE_CXX_COMPILER "/opt/homebrew/Cellar/gcc/15.1.0/bin/g++-15")


project(loong-boot C CXX)

# 确保始终设置 CMAKE_BUILD_TYPE
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type (default Release)" FORCE)
    message(WARNING "未设置 CMAKE_BUILD_TYPE，默认使用 Release 模式")
endif()

include(CheckIncludeFileCXX)

# 检查 C++ 特性支持
check_include_file_cxx(any HAS_ANY)
check_include_file_cxx(string_view HAS_STRING_VIEW)
check_include_file_cxx(coroutine HAS_COROUTINE)

# 设置 C++26 标
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(FOO_USE_STATIC_LIBS ON)
set(CMAKE_BUILD_PARALLEL_LEVEL 8)

# 可配置的优化选项
option(AGGRESSIVE_OPT "启用激进优化" OFF)
option(USE_LTO "启用链接时优化" OFF)
option(USE_OPENMP "启用 OpenMP 并行化" OFF)
option(USE_WNO "关闭警告" ON)

option(USE_PGO_GEN "启用 PGO 生成阶段" OFF)
option(USE_PGO_USE "启用 PGO 使用阶段" OFF)

# 设置链接路径
add_link_options(-L/usr/local/lib)

# 查找 Protobuf
#[[find_package(Protobuf REQUIRED)
include_directories(${Protobuf_INCLUDE_DIRS})
add_subdirectory(proto)
include_directories(${PROTO_GEN_C_INCLUDE_DIRS} ${PROTO_GEN_CXX_INCLUDE_DIRS})]]

# 源文件
set(MODEL_SOURCES_DTO base/dto/open_dto.h)
set(MODEL_SOURCES_VO base/vo/data_vo.h)
set(MODEL_SOURCES_COMMON
        aop/Application.h
        threadPool/threadPool.h
        utils/checkloginUtils.h
        filters/LoginFilter.h
        utils/opensslCrypto.h
        filters/SqlFilter.h
        kafkaManager/AsyncKafkaConsumer.h
        base/base.h
        mqttManager/MqttManager.h
        mqttManager/MqttConsumer.h
        kafkaManager/AsyncKafkaConsumerOne.h
        utils/retry_utils.h
        utils/tbbUtils.h
        coroutinePool/TbbCoroutinePool.h
        coroutinePool/TbbCoroutinePool.cc
)

add_executable(
        ${PROJECT_NAME}
        ${MODEL_SOURCES_DTO}
        ${MODEL_SOURCES_VO}
        ${MODEL_SOURCES_COMMON}
        main.cc
        utils/sql.h
        controllers/PlaintextCtrl.cc
        controllers/PlaintextCtrl.h
        controllers/api_v1_User.cc
        controllers/api_v1_User.h
        controllers/api_v1_OpenApi.cc
        controllers/api_v1_OpenApi.h
        utils/redisUtils.h
        utils/redisUtils.cpp
        ws_controllers/ChatWebsocket.h
        ws_controllers/ChatWebsocket.cc
        service/SbcConvertService.h
        service/impl/SbcConvertServiceImpl.cpp
        service/TrieService.h
        service/impl/TrieServiceImpl.cpp
)

if(NOT USE_PGO_GEN)
    set_target_properties(${PROJECT_NAME} PROPERTIES INTERPROCEDURAL_OPTIMIZATION ${USE_LTO})
endif()

# 对于 PGO 使用阶段：
if(USE_PGO_USE)
    target_compile_options(${PROJECT_NAME} PRIVATE
            -fprofile-use
    )
    target_link_options(${PROJECT_NAME} PRIVATE -fprofile-use)
endif()

# 对于 PGO 生成阶段：
if(USE_PGO_GEN)
    target_compile_options(${PROJECT_NAME} PRIVATE
            -fprofile-generate
    )
    target_link_options(${PROJECT_NAME} PRIVATE -fprofile-generate)
endif()

#关闭警告
if(USE_WNO)
    add_compile_options(
            -Wno-missing-requires
            -Wno-deprecated-declarations
            -Wno-stringop-overread
    )
endif()

# 检测目标架构并定义宏
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm)")
    add_compile_definitions(ARM_ARCH)
    target_compile_options(${PROJECT_NAME} PRIVATE -mcpu=native -mtune=native)
    message(STATUS "为 ARM 架构配置")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)")
    add_compile_definitions(DX86_ARCH)
    target_compile_options(${PROJECT_NAME} PRIVATE -march=native -mtune=native -mfma -msse4.2 -mpopcnt -mavx2)
    message(STATUS "为 x86_64 架构配置")
else()
    message(WARNING "不支持的架构: ${CMAKE_SYSTEM_PROCESSOR}，使用通用设置")
endif()

# 激进优化选项
if(AGGRESSIVE_OPT)
    target_compile_options(${PROJECT_NAME} PRIVATE
        -fno-stack-protector
        -fweb
        -fgcse-after-reload
        -fdelete-null-pointer-checks
        --param=inline-unit-growth=1500
        --param=max-inline-insns-single=2500
        --param=large-function-growth=1500
        --param=max-inline-recursive-depth=12
    )
endif()

# 链接时优化（LTO）选项
if(USE_LTO AND NOT USE_PGO_GEN)
    target_compile_options(${PROJECT_NAME} PRIVATE -flto=auto -ffat-lto-objects)
endif()

# OpenMP 并行化选项
if(USE_OPENMP)
    set(OPENMP_FLAGS "-fopenmp")
else()
    set(OPENMP_FLAGS "")
endif()

# 针对不同构建类型的编译选项
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(${PROJECT_NAME} PRIVATE -pipe -Wall -Wextra -Wno-unused-parameter -Wno-uninitialized -Wno-unused-function -Wno-deprecated-declarations -Wno-missing-requires -Og -fsanitize=leak)
    message(STATUS "调试模式: 启用 -Og -fsanitize=leak")
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(${PROJECT_NAME} PRIVATE -pipe -Wno-unused-parameter -Wno-uninitialized -Wno-unused-function -Wno-deprecated-declarations -Wno-missing-requires -Ofast -fipa-pta -freorder-blocks -finline-functions -falign-functions=32 -falign-loops=32 -funroll-loops ${AGGRESSIVE_FLAGS} ${LTO_FLAGS} ${OPENMP_FLAGS})
    message(STATUS "发布模式: 启用 -Ofast 等优化")
# 这里不再有 else 分支，因为 CMAKE_BUILD_TYPE 已确保被设置
endif()

# 拉取 Glaze 稳定版本
include(FetchContent)
FetchContent_Declare(
        glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG main  # 替换为最新版本，例如 v6.0.0
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glaze)

# 查找其他依赖
find_package(Drogon CONFIG REQUIRED)
find_package(mimalloc 3.1.5 REQUIRED)
find_package(Boost 1.88.0 REQUIRED)
find_package(TBB REQUIRED)

# 链接库
target_link_libraries(${PROJECT_NAME} PRIVATE
        Drogon::Drogon
        Boost::boost
        glaze::glaze
        kafka-core
        rdkafka
        TBB::tbb
        mimalloc
        #[[proto_gen_cxx]]
)

# 编译定义
target_compile_definitions(${PROJECT_NAME} PRIVATE MI_MALLOC=1)

# 启用 ccache
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

# 添加其他源文件
aux_source_directory(controllers CTL_SRC)
aux_source_directory(filters FILTER_SRC)
aux_source_directory(plugins PLUGIN_SRC)
aux_source_directory(models MODEL_SRC)

# Drogon 视图
drogon_create_views(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/views ${CMAKE_CURRENT_BINARY_DIR})

# 包含目录
target_include_directories(${PROJECT_NAME}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/models
        ${CMAKE_CURRENT_SOURCE_DIR}/external/kafka-core/include
)

# 源文件
target_sources(${PROJECT_NAME}
        PRIVATE
        ${SRC_DIR}
        ${CTL_SRC}
        ${FILTER_SRC}
        ${PLUGIN_SRC}
        ${MODEL_SRC}
)

# === 构建脚本提示（PGO） ===
# 使用如下命令手动构建生成 profile 数据：
# sh ./build.sh gen             # 使用 Release 模式生成 PGO 数据
# ./build-pgo-gen/loong-boot # 运行一次完整业务
# sh ./build.sh use             # 使用 Release 模式构建优化版本

# 测试子目录
add_subdirectory(test)