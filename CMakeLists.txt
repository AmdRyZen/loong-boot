cmake_minimum_required(VERSION 3.31.6)

# 检查 CMake 版本，并根据版本设置政策
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
else()
    message(STATUS "CMake version does not support policy CMP0167")
endif()

SET(CMAKE_C_COMPILER "/opt/homebrew/Cellar/gcc/15.1.0/bin/gcc-15")
SET(CMAKE_CXX_COMPILER "/opt/homebrew/Cellar/gcc/15.1.0/bin/g++-15")

project(loong-boot C CXX)

include(CheckIncludeFileCXX)

#configure_file(config.h.in ${CMAKE_CURRENT_SOURCE_DIR}/config.h.in @ONLY)

check_include_file_cxx(any HAS_ANY)
check_include_file_cxx(string_view HAS_STRING_VIEW)
check_include_file_cxx(coroutine HAS_COROUTINE)
if (NOT "${CMAKE_CXX_STANDARD}" STREQUAL "")
    # Do nothing
elseif (HAS_ANY AND HAS_STRING_VIEW AND HAS_COROUTINE)
    set(CMAKE_CXX_STANDARD 26)

elseif (HAS_ANY AND HAS_STRING_VIEW)
    set(CMAKE_CXX_STANDARD 26)
else ()
    set(CMAKE_CXX_STANDARD 26)
endif ()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    add_compile_options(-DARM_ARCH)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86")
    add_compile_options(-DX86_ARCH)
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 条件判断 release  -flto   -flto=thin  -ljemalloc -mfpu=neon -static-libgcc -static-libstdc++
# debug clang -fsanitize=address  gcc -fsanitize=leak
if (CMAKE_BUILD_TYPE AND (CMAKE_BUILD_TYPE STREQUAL "Debug"))
    # 第一步：生成代码覆盖率文件
    #[[set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-generate=${CMAKE_SOURCE_DIR}/fprofile")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-generate=${CMAKE_SOURCE_DIR}/fprofile")]]

    set(CMAKE_CXX_FLAGS_DEBUG "-pipe -Wall -Og -fsanitize=leak -L/usr/local/lib")
    message("Debug mode:${CMAKE_CXX_FLAGS_DEBUG}")
elseif (CMAKE_BUILD_TYPE AND (CMAKE_BUILD_TYPE STREQUAL "Release"))
    # 第二步：使用代码覆盖率文件
   #[[ set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-use=${CMAKE_SOURCE_DIR}/fprofile")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-use=${CMAKE_SOURCE_DIR}/fprofile")]]

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
        set(CMAKE_CXX_FLAGS_RELEASE "-pipe -w -Ofast -fomit-frame-pointer -mcpu=native -mtune=native -ftree-vectorize \
        -ftree-vectorize -fopenmp -ftree-parallelize-loops=3 -fipa-pta -freorder-blocks-and-partition \
        -fmerge-all-constants -floop-unroll-and-jam -fno-strict-overflow -fno-tree-loop-distribute-patterns \
        -ffast-math -falign-functions=32 -falign-labels=8 -falign-loops=32 -freorder-blocks -fweb \
        -fdelete-null-pointer-checks \
        -fno-stack-protector -finline-functions -funroll-loops -L/usr/local/lib -DARM_ARCH")
        message("Release mode for ARM:${CMAKE_CXX_FLAGS_RELEASE}")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86")
        set(CMAKE_CXX_FLAGS_RELEASE "-pipe -w -Ofast -flto=auto \
        -ftree-vectorize -ffat-lto-objects \
        -funroll-loops -fgcse-after-reload -frename-registers \
        -fipa-cp-clone -fwhole-program -fstrict-aliasing \
        -fstrict-overflow -finline-limit=1000 -freorder-blocks-and-partition \
        -fsplit-loops -fpredictive-commoning -fgcse-las \
        -fdelete-null-pointer-checks -fhoist-adjacent-loads \
        -fmerge-constants -fivopts -floop-interchange \
        -floop-strip-mine -floop-block -floop-unroll-and-jam \
        -fpeel-loops -fprefetch-loop-arrays -fauto-profile \
        -fprofile-use -fprofile-correction -foptimize-sibling-calls \
        -foptimize-strlen -freorder-functions -freorder-blocks \
        -ftracer -fweb -fipa-ra -fipa-cp \
        -fipa-pta -fipa-icf -fdevirtualize-speculatively \
        -fipa-devirt -fsched-spec -fsched-pressure \
        -fsched-last-insn-heuristic -fsched-rank-heuristic \
        -fsched-group-heuristic -fsched-dep-count-heuristic \
        -fmodulo-sched -fmodulo-sched-allow-regmoves \
        -fbranch-target-load-optimize2 -fcx-limited-range \
        -funsafe-math-optimizations -ffinite-math-only \
        -fno-trapping-math -ffp-contract=fast -fno-rounding-math \
        -fno-signaling-nans -march=native -mtune=native \
        -mfma -msse4.2 -mpopcnt -mavx2 -flax-vector-conversions \
        -Wall -Wextra -Wpedantic -Wno-unused-parameter \
        -Wno-unused-variable -Wno-unused-function -Wno-unused-result \
        -Wno-sign-compare -Wno-unused-but-set-variable -Wno-maybe-uninitialized \
        -Wno-strict-overflow -Wno-array-bounds -Wno-address-of-packed-member \
        -Wno-aggregate-return -Wno-attributes -Wno-unused-but-set-label \
        -Wno-unused-value -Wno-ignored-qualifiers -Wno-psabi -Wno-error \
        -Wl,--strip-all -Wl,--gc-sections -fuse-ld=gold \
        -L/usr/local/lib -DX86_ARCH")
        message("Release mode for x86:${CMAKE_CXX_FLAGS_RELEASE}")
    else()
        message(FATAL_ERROR "Unsupported architecture")
    endif()
else ()
    set(CMAKE_CXX_FLAGS_RELEASE "-pipe -w -Ofast -fomit-frame-pointer -march=native -mtune=native -L/usr/local/lib")
    message("Release mode:${CMAKE_CXX_FLAGS_RELEASE}")
    message("CMAKE_BUILD_TYPE:${CMAKE_BUILD_TYPE}")
endif ()

set(FOO_USE_STATIC_LIBS ON) # 告诉CMake优先使用静态库
#set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++") # 对于GCC

#find_package(Threads REQUIRED)
#find_package(PkgConfig REQUIRED)
# Find required protobuf package

find_package(Protobuf REQUIRED)
if (PROTOBUF_FOUND)
    message(STATUS "protobuf library found")
else ()
    message(FATAL_ERROR "protobuf library is needed but cant be found")
endif ()
# 添加 Protobuf 的库目录
link_directories(/usr/local/lib)
include_directories(${Protobuf_INCLUDE_DIRS})
add_subdirectory(proto)
include_directories(${PROTO_GEN_C_INCLUDE_DIRS}) # c
include_directories(${PROTO_GEN_CXX_INCLUDE_DIRS}) # cpp

set(MODEL_SOURCES_DTO
        base/dto/open_dto.h
)

set(MODEL_SOURCES_VO
        base/vo/data_vo.h
)

set(MODEL_SOURCES_COMMON
        aop/Application.h
        threadPool/threadPool.h utils/checkloginUtils.h
        filters/LoginFilter.h utils/opensslCrypto.h
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
        ${PROJECT_NAME} ${MODEL_SOURCES_DTO} ${MODEL_SOURCES_VO} ${MODEL_SOURCES_COMMON}
        main.cc utils/sql.h
        controllers/PlaintextCtrl.cc controllers/PlaintextCtrl.h
        controllers/api_v1_User.cc controllers/api_v1_User.h
        controllers/api_v1_OpenApi.cc controllers/api_v1_OpenApi.h
        utils/redisUtils.h utils/redisUtils.cpp
        ws_controllers/ChatWebsocket.h ws_controllers/ChatWebsocket.cc
        service/SbcConvertService.h service/impl/SbcConvertServiceImpl.cpp
        service/TrieService.h service/impl/TrieServiceImpl.cpp
)


# ##############################################################################
# If you include the drogon source code locally in your project, use this method
# to add drogon
# add_subdirectory(drogon)
# target_link_libraries(${PROJECT_NAME} PRIVATE drogon)
#
# and comment out the following lines

include(FetchContent)
FetchContent_Declare(
        glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG main
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glaze)

# 下载和构建 librdkafka
#[[FetchContent_Declare(
        librdkafka
        GIT_REPOSITORY https://github.com/confluentinc/librdkafka.git
        #GIT_TAG master
        GIT_TAG v2.10.0  # 使用稳定版本而非 master，master 可能不稳定
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(librdkafka)]]

# 添加 KafkaCore 子目录
# add_subdirectory(external/kafka-core)
find_library(KAFKA_CORE_LIB kafka-core PATHS /usr/local/lib)
find_path(KAFKA_CORE_INCLUDE_DIR kafka/KafkaManager.h PATHS /usr/local/include)

find_package(Drogon CONFIG REQUIRED)
find_package(mimalloc 2.1.7 REQUIRED)
find_package(Boost 1.86.0 REQUIRED)
find_package(TBB REQUIRED)
#[[find_package(PahoMqttCpp REQUIRED)]]
target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon Boost::boost glaze::glaze kafka-core rdkafka TBB::tbb #[[PahoMqttCpp::paho-mqttpp3]] mimalloc proto_gen_cxx)
target_compile_definitions(${PROJECT_NAME} PRIVATE MI_MALLOC=1)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

# ##############################################################################

if (CMAKE_CXX_STANDARD LESS 17)
    # With C++14, use boost to support any, string_view and filesystem
    message(STATUS "use c++14")
    #find_package(Boost 1.81.0 REQUIRED)
    #target_link_libraries(${PROJECT_NAME} PUBLIC Boost::boost)
elseif (CMAKE_CXX_STANDARD LESS 23)
    message(STATUS "use c++20")
elseif (CMAKE_CXX_STANDARD LESS 26)
    message(STATUS "use c++23")
else ()
    message(STATUS "use c++26")
endif ()

aux_source_directory(controllers CTL_SRC)
aux_source_directory(filters FILTER_SRC)
aux_source_directory(plugins PLUGIN_SRC)
aux_source_directory(models MODEL_SRC)

drogon_create_views(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/views
        ${CMAKE_CURRENT_BINARY_DIR})
# use the following line to create views with namespaces.
# drogon_create_views(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/views
#                     ${CMAKE_CURRENT_BINARY_DIR} TRUE)

target_include_directories(${PROJECT_NAME}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/models
        ${CMAKE_CURRENT_SOURCE_DIR}/external/kafka-core/include)
target_sources(${PROJECT_NAME}
        PRIVATE
        ${SRC_DIR}
        ${CTL_SRC}
        ${FILTER_SRC}
        ${PLUGIN_SRC}
        ${MODEL_SRC})
# ##############################################################################
# uncomment the following line for dynamically loading views
# set_property(TARGET ${PROJECT_NAME} PROPERTY ENABLE_EXPORTS ON)

# ##############################################################################

add_subdirectory(test)
