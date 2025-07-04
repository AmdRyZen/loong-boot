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

include(CheckIncludeFileCXX)

# 检查 C++ 特性支持
check_include_file_cxx(any HAS_ANY)
check_include_file_cxx(string_view HAS_STRING_VIEW)
check_include_file_cxx(coroutine HAS_COROUTINE)

# 设置 C++26 标准
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# 优先使用静态库
set(FOO_USE_STATIC_LIBS ON)

# 查找 Protobuf
add_compile_options(-Wno-deprecated-declarations)
add_compile_options(-Wno-missing-requires)
find_package(Protobuf REQUIRED)
include_directories(${Protobuf_INCLUDE_DIRS})
add_subdirectory(proto)  # 保留，如果你有 proto 子目录
include_directories(${PROTO_GEN_C_INCLUDE_DIRS} ${PROTO_GEN_CXX_INCLUDE_DIRS})

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
        coroutinePool/CoroutinePool.h
        coroutinePool/CoroutinePool.cc
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

# 拉取 Glaze 稳定版本
include(FetchContent)
FetchContent_Declare(
        glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG main  # 请检查 GitHub 获取最新稳定版本
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glaze)

# 查找其他依赖
find_package(Drogon CONFIG REQUIRED)
find_package(mimalloc 2.1.7 REQUIRED)
find_package(Boost 1.86.0 REQUIRED)
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
        proto_gen_cxx
)

# 编译定义
target_compile_definitions(${PROJECT_NAME} PRIVATE MI_MALLOC=1)
# 适用于 Clang 或 GCC
#add_link_options(-fuse-ld=mold)

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

# 测试子目录
add_subdirectory(test)

# 架构特定的宏定义
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm)")
    add_compile_options(-DARM_ARCH)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)")
    add_compile_options(-DX86_ARCH)
else()
    message(WARNING "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# 编译选项（极限性能）
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS_DEBUG "-pipe -Wall -Wextra -Og -fsanitize=leak -L/usr/local/lib")
    message(STATUS "Debug mode: ${CMAKE_CXX_FLAGS_DEBUG}")
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm)")
        set(CMAKE_CXX_FLAGS_RELEASE
                "-pipe -Ofast -mcpu=native -mtune=native -flto=auto -ffat-lto-objects -fopenmp -fipa-pta -freorder-blocks -fmerge-constants -finline-functions -falign-functions=32 -falign-loops=32 -funroll-loops -fgcse-after-reload -fdelete-null-pointer-checks -fweb -fno-stack-protector -L/usr/local/lib -DARM_ARCH -Wall -Wextra -Wno-unused-parameter -Wno-uninitialized -Wno-unused-function --param=max-inline-insns-single=5000 --param=inline-unit-growth=150 --param=large-function-growth=200 --param=max-inline-recursive-depth=10"
        )
        message(STATUS "Release mode for ARM: ${CMAKE_CXX_FLAGS_RELEASE}")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)")
        set(CMAKE_CXX_FLAGS_RELEASE
                "-pipe -Ofast -march=native -mtune=native -flto=auto -ffat-lto-objects -fipa-pta -freorder-blocks -fmerge-constants -finline-functions -funroll-loops -fgcse-after-reload -fdelete-null-pointer-checks -fweb -fno-stack-protector -mfma -msse4.2 -mpopcnt -mavx2 -L/usr/local/lib -DX86_ARCH -Wall -Wextra -Wno-unused-parameter -Wno-uninitialized -Wno-unused-function --param=max-inline-insns-single=5000 --param=inline-unit-growth=150 --param=large-function-growth=200 --param=max-inline-recursive-depth=10"
        )
        message(STATUS "Release mode for x86: ${CMAKE_CXX_FLAGS_RELEASE}")
    else()
        set(CMAKE_CXX_FLAGS_RELEASE "-pipe -Ofast -march=native -mtune=native -flto=auto -L/usr/local/lib")
        message(STATUS "Release mode: ${CMAKE_CXX_FLAGS_RELEASE}")
    endif()
else()
    message(WARNING "CMAKE_BUILD_TYPE not set, defaulting to Release")
    set(CMAKE_CXX_FLAGS_RELEASE "-pipe -Ofast -march=native -mtune=native -flto=auto -L/usr/local/lib")
    message(STATUS "Release mode: ${CMAKE_CXX_FLAGS_RELEASE}")
endif()