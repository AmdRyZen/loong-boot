#!/bin/bash
set -e

# ⚠️ 不要用 sudo 跑整个脚本！
#   编译阶段一旦以 root 身份跑，build/ 里所有产物都会变成 root 所有，
#   之后不带 sudo 的增量构建就再也删不掉/改不动它们（只能 sudo rm -rf 收拾）。
#   只有最后一步 `cmake --install`（往 /usr/local 写）需要 root，脚本自己会 sudo。
if [ "$(id -u)" -eq 0 ]; then
    echo "错误：请以普通用户运行本脚本（不要 sudo）。"
    echo "      脚本最后一步会用 sudo 安装到 /usr/local，届时输入密码即可。"
    exit 1
fi

# build/ 若属于其他用户（历史上用 sudo 跑过），当前用户删不动 —— 先用 sudo 清掉。
if [ -d build ] && [ ! -w build ]; then
    echo "build/ 不属于当前用户（多半是之前用 sudo 跑过整个脚本），正在清理…"
    sudo rm -rf build
fi

# ─────────────────────────────────────────────────────────────────────────────
# kafka-core 本地静态库的构建与安装。
#
# ⚠️ 四个必须处理的坑（都是实测踩过的，别再简化掉）：
#
# 1) 编译器必须显式指定。
#    CMake 缓存（build/CMakeCache.txt）里的 CMAKE_CXX_COMPILER 优先级【高于】
#    CMakeLists.txt 里的 set()。本机缓存里钉的还是已被 brew 升级移除的 gcc-15，
#    只改 CMakeLists 而不覆盖缓存，configure 会一直失败：
#      "is not a full path to an existing compiler tool"
#    现象是「改了 src/ 却怎么都装不上」，很容易误判成代码没生效。
#
# 2) OpenSSL 必须显式给 root。
#    librdkafka 的 CMakeLists 会做 get_target_property(OpenSSL::SSL)。若
#    FindOpenSSL 找到的版本与缓存里的老 openssl 路径不一致，就会报：
#      get_target_property() called with non-existent target "OpenSSL::SSL"
#    这是缓存陈旧导致的，指定 OPENSSL_ROOT_DIR 即可。
#
# 3) 【构建阶段绝对不能用 sudo】—— 这是 2026-09-15 踩的最大的一个坑：
#      fatal error: stdio.h: No such file or directory
#      fatal error: errno.h: No such file or directory
#    原因链：
#      · Homebrew 的 gcc-16 是【烤死】了 sysroot 的：
#          --with-sysroot=/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk
#      · 但这个 SDK 在本机【不存在】（本机只有 MacOSX14.5.sdk / MacOSX15.4.sdk）。
#      · 平时能编译，是因为 shell 里导出了 SDKROOT=/Library/.../MacOSX.sdk，
#        Darwin driver 会优先读 SDKROOT，从而覆盖那个不存在的烤死路径。
#      · 而 `sudo` 默认 env_reset 会把 SDKROOT 抹掉 ⇒ gcc 回退到不存在的
#        烤死 sysroot ⇒ 连 stdio.h 都找不到。
#    结论：**构建以当前用户跑，只有「写 /usr/local」这一步才需要 sudo。**
#    为彻底免疫环境差异，下面再把 sysroot 显式塞进 C/CXX flags。
#    （CMake 只对 Apple clang 自动加 -isysroot；对 Homebrew gcc 不会加，
#      所以 CMAKE_OSX_SYSROOT 在 flags.make 里是【看不见的】，别指望它。）
#
# 4) 因此这里直接【清掉 build/】重新配置 —— 上面 1、2 两个问题都源于陈旧缓存。
#    代价是要重新 FetchContent librdkafka（本机实测约 12 秒，可接受），
#    换来的是「脚本永远能跑通」，而不是让人去猜缓存里还钉着什么。
# ─────────────────────────────────────────────────────────────────────────────

CXX_BIN="$(command -v g++-16 || command -v g++-15 || command -v g++)"
CC_BIN="$(command -v gcc-16 || command -v gcc-15 || command -v gcc)"

# ── OpenSSL：优先 openssl@4，其次 openssl@3 ──────────────────────────────────
#
# ⚠️ 这里必须让「头文件」和「库」来自同一个 OpenSSL，否则 librdkafka 链接必炸：
#      Undefined symbols: _BIO_eof / _OPENSSL_sk_set_thunks
#    原因：CMake 的 find_package 认 OPENSSL_ROOT_DIR，但 librdkafka 还会把
#    pkg-config 的 _OPENSSL_CFLAGS 拼进 include 路径。只钉 OPENSSL_ROOT_DIR 时，
#    find_package 拿的是 @3 的库，pkg-config 却按 /opt/homebrew 默认（现为 @4）
#    给头文件 ⇒ 用 4.x 头编译、链 3.x 库 ⇒ 4.x 才成为真实函数的 BIO_eof /
#    OPENSSL_sk_set_thunks 找不到定义。
#    所以：OPENSSL_ROOT_DIR 与 PKG_CONFIG_PATH 必须一起指到同一个前缀。
OPENSSL_ROOT=""
if command -v brew >/dev/null 2>&1; then
    for cand in openssl@4 openssl@3; do
        p="$(brew --prefix "${cand}" 2>/dev/null || true)"
        if [ -n "${p}" ] && [ -d "${p}" ]; then
            OPENSSL_ROOT="${p}"
            break
        fi
    done
fi
if [ -z "${OPENSSL_ROOT}" ]; then
    for cand in /opt/homebrew/opt/openssl@4 /opt/homebrew/opt/openssl@3; do
        if [ -d "${cand}" ]; then OPENSSL_ROOT="${cand}"; break; fi
    done
fi
if [ -n "${OPENSSL_ROOT}" ] && [ -d "${OPENSSL_ROOT}/lib/pkgconfig" ]; then
    export PKG_CONFIG_PATH="${OPENSSL_ROOT}/lib/pkgconfig:${PKG_CONFIG_PATH}"
fi

# 显式解析 SDK sysroot（不依赖 SDKROOT 环境变量，也不依赖 CMake 是否认识编译器）
SDK_PATH="$(xcrun --show-sdk-path 2>/dev/null || true)"
if [ -z "${SDK_PATH}" ] || [ ! -d "${SDK_PATH}" ]; then
    for cand in /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk \
                /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk; do
        if [ -d "${cand}" ]; then SDK_PATH="${cand}"; break; fi
    done
fi

echo "using C compiler:   ${CC_BIN}"
echo "using C++ compiler: ${CXX_BIN}"
echo "using OpenSSL root: ${OPENSSL_ROOT:-<system default>}"
echo "using SDK sysroot:  ${SDK_PATH:-<none, 让编译器自己找>}"

rm -rf build

cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DCMAKE_C_COMPILER="${CC_BIN}" -DCMAKE_CXX_COMPILER="${CXX_BIN}" \
      ${OPENSSL_ROOT:+-DOPENSSL_ROOT_DIR="${OPENSSL_ROOT}"} \
      ${SDK_PATH:+-DCMAKE_OSX_SYSROOT="${SDK_PATH}"} \
      ${SDK_PATH:+-DCMAKE_C_FLAGS="-isysroot ${SDK_PATH}"} \
      ${SDK_PATH:+-DCMAKE_CXX_FLAGS="-isysroot ${SDK_PATH}"}

# 构建：当前用户身份跑（保留 SDKROOT 等环境，见上面第 3 条）
cmake --build build -j8

# 安装：只有这一步需要 root（往 /usr/local 写文件），不涉及编译
sudo cmake --install build

echo "done: libkafka-core.a installed to /usr/local"
