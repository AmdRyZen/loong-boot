cmake_minimum_required(VERSION 4.1)

# 检查 CMake 版本，设置政策
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
else()
    message(STATUS "CMake version does not support policy CMP0167")
endif()

# 设置编译器（移除硬编码路径，允许用户自定义）
if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "/opt/homebrew/Cellar/gcc/15.2.0/bin/gcc-15")
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "/opt/homebrew/Cellar/gcc/15.2.0/bin/g++-15")
endif()

project(loong-boot C CXX)

# 确保始终设置 CMAKE_BUILD_TYPE
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type (default Release)" FORCE)
    message(WARNING "未设置 CMAKE_BUILD_TYPE，默认使用 Release 模式")
endif()

# 设置 C++26 标准
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_BUILD_PARALLEL_LEVEL 8)

# 检查特性支持
include(CheckIncludeFileCXX)
check_include_file_cxx(any HAS_ANY)
check_include_file_cxx(string_view HAS_STRING_VIEW)
check_include_file_cxx(coroutine HAS_COROUTINE)

# 可配置的优化选项
option(AGGRESSIVE_OPT "启用激进优化" OFF)
option(USE_LTO "启用链接时优化" OFF)
option(USE_OPENMP "启用 OpenMP 并行化" OFF)
option(USE_WNO "关闭特定警告" ON)
option(USE_PGO_GEN "启用 PGO 生成阶段" OFF)
option(USE_PGO_USE "启用 PGO 使用阶段" OFF)

# 设置编译选项
set(COMMON_COMPILE_OPTIONS
        $<$<CONFIG:Debug>:-pipe;-Wall;-Wextra;-Wno-unused-parameter;-Wno-uninitialized;-Wno-unused-function;-Wno-deprecated-declarations;-Wno-missing-requires;-Og;-fsanitize=leak>
        $<$<CONFIG:Release>:-pipe;-Wno-unused-parameter;-Wno-uninitialized;-Wno-unused-function;-Wno-deprecated-declarations;-Wno-missing-requires;-Ofast;-fipa-pta;-freorder-blocks;-finline-functions;-falign-functions=32;-falign-loops=32;-funroll-loops>
)

# 关闭警告选项
if(USE_WNO)
    list(APPEND COMMON_COMPILE_OPTIONS
            -Wno-missing-requires
            -Wno-deprecated-declarations
            -Wno-stringop-overread
    )
endif()

# 检测目标架构并定义宏
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|arm)")
    add_compile_definitions(ARM_ARCH)
    list(APPEND COMMON_COMPILE_OPTIONS -mcpu=native -mtune=native)
    message(STATUS "为 ARM 架构配置")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)")
    add_compile_definitions(X86_64_ARCH)
    list(APPEND COMMON_COMPILE_OPTIONS -march=native -mtune=native -mfma -msse4.2 -mpopcnt -mavx2)
    message(STATUS "为 x86_64 架构配置")
else()
    message(WARNING "不支持的架构: ${CMAKE_SYSTEM_PROCESSOR}，使用通用设置")
endif()

# 激进优化选项
if(AGGRESSIVE_OPT)
    set(AGGRESSIVE_FLAGS
            -fno-stack-protector
            -fweb
            -fgcse-after-reload
            -fdelete-null-pointer-checks
            --param=inline-unit-growth=1500
            --param=max-inline-insns-single=2500
            --param=large-function-growth=1500
            --param=max-inline-recursive-depth=12
    )
    list(APPEND COMMON_COMPILE_OPTIONS ${AGGRESSIVE_FLAGS})
endif()

# OpenMP 支持
if(USE_OPENMP)
    find_package(OpenMP REQUIRED)
    set(OPENMP_FLAGS ${OpenMP_CXX_FLAGS})
else()
    set(OPENMP_FLAGS "")
endif()

# LTO 选项
if(USE_LTO AND NOT USE_PGO_GEN)
    list(APPEND COMMON_COMPILE_OPTIONS -flto=auto -ffat-lto-objects)
    set(LTO_LINK_OPTIONS -flto=auto)
endif()

# PGO 选项
if(USE_PGO_GEN)
    list(APPEND COMMON_COMPILE_OPTIONS -fprofile-generate)
    list(APPEND LTO_LINK_OPTIONS -fprofile-generate)
elseif(USE_PGO_USE)
    list(APPEND COMMON_COMPILE_OPTIONS -fprofile-use)
    list(APPEND LTO_LINK_OPTIONS -fprofile-use)
endif()

# 创建目标
add_executable(${PROJECT_NAME})

# 设置源文件
set(MODEL_SOURCES_DTO
        base/dto/open_dto.h
)
set(MODEL_SOURCES_VO
        base/vo/data_vo.h
)
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

# 添加源文件到目标
target_sources(${PROJECT_NAME} PRIVATE
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
        ${MODEL_SOURCES_DTO}
        ${MODEL_SOURCES_VO}
        ${MODEL_SOURCES_COMMON}
)

# 收集其他源文件
file(GLOB_RECURSE CTL_SOURCES "controllers/*.cc" "controllers/*.h")
file(GLOB_RECURSE FILTER_SOURCES "filters/*.cc" "filters/*.h")
file(GLOB_RECURSE PLUGIN_SOURCES "plugins/*.cc" "plugins/*.h")
file(GLOB_RECURSE MODEL_SOURCES "models/*.cc" "models/*.h")

target_sources(${PROJECT_NAME} PRIVATE
        ${CTL_SOURCES}
        ${FILTER_SOURCES}
        ${PLUGIN_SOURCES}
        ${MODEL_SOURCES}
)

# 设置编译选项
target_compile_options(${PROJECT_NAME} PRIVATE ${COMMON_COMPILE_OPTIONS})

# 设置链接选项
if(DEFINED LTO_LINK_OPTIONS)
    target_link_options(${PROJECT_NAME} PRIVATE ${LTO_LINK_OPTIONS})
endif()

# 添加链接路径
target_link_directories(${PROJECT_NAME} PRIVATE /usr/local/lib)

# 拉取 Glaze 稳定版本
include(FetchContent)
FetchContent_Declare(
        glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG main  # 建议替换为具体稳定版本号
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glaze)

# 查找其他依赖
find_package(Drogon CONFIG REQUIRED)
find_package(mimalloc 3.1.5 REQUIRED)
find_package(Boost 1.89.0 REQUIRED)
find_package(TBB REQUIRED)

# 检查 Homebrew 路径是否存在
if(EXISTS "/opt/homebrew/include")
    target_include_directories(${PROJECT_NAME} PRIVATE /opt/homebrew/include)
endif()

# 链接库
target_link_libraries(${PROJECT_NAME} PRIVATE
        Drogon::Drogon
        Boost::boost
        glaze::glaze
        kafka-core
        rdkafka
        TBB::tbb
        mimalloc
        $<$<BOOL:${USE_OPENMP}>:OpenMP::OpenMP_CXX>
)

# 编译定义
target_compile_definitions(${PROJECT_NAME} PRIVATE MI_MALLOC=1)

# 启用 ccache
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

# 包含目录
target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/models
        ${CMAKE_CURRENT_SOURCE_DIR}/external/kafka-core/include
)

# 处理测试子目录
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
    add_subdirectory(test)
endif()

# 可选：为不同构建类型设置属性
set_target_properties(${PROJECT_NAME} PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED ${CMAKE_CXX_STANDARD_REQUIRED}
        CXX_EXTENSIONS ${CMAKE_CXX_EXTENSIONS}
)

# 显示构建信息
message(STATUS "Project: ${PROJECT_NAME}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Architecture: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Optimizations: AGGRESSIVE_OPT=${AGGRESSIVE_OPT}, LTO=${USE_LTO}, OPENMP=${USE_OPENMP}")
message(STATUS "PGO: GEN=${USE_PGO_GEN}, USE=${USE_PGO_USE}")
