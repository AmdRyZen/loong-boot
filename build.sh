#!/usr/bin/env bash

#cmake - DCMAKE_BUILD_TYPE = Release - B./ build && cmake-- build build - j4-- config Release

# PGO and performance options
use_pgo=OFF
gen_pgo=OFF
aggressive_opt=OFF
use_lto=OFF
use_native=ON
use_fast_linker=OFF
run_after_build=ON
build_dir='./build'
profile_dir=''

function build_loong()
{
    #clang - format - style = file - i controllers/*

    #Update the submodule and initialize
    #git submodule update --init
    # swiftc -O test.swift -o bench_swift
    # cjc -O2 --experimental --target-cpu=native test.cj -o bench_cj


    if [ -z "$1" ]; then
        set -- Release "$@"
    fi

    echo "build_loong build_type=$1"

    #Save current directory
    current_dir="${PWD}"

    if [[ -z "$profile_dir" ]]; then
        profile_dir="${current_dir}/build-pgo-gen/pgo-data"
    fi

    #Create building folder
    echo "Created building folder: ${build_dir}"
    mkdir -p $build_dir

    echo "Entering folder: ${build_dir}"
    cd $build_dir || exit

    cmake_common_options=(
        -DUSE_PGO_GEN=${gen_pgo}
        -DUSE_PGO_USE=${use_pgo}
        -DAGGRESSIVE_OPT=${aggressive_opt}
        -DUSE_LTO=${use_lto}
        -DUSE_NATIVE_ARCH=${use_native}
        -DUSE_FAST_LINKER=${use_fast_linker}
        -DPGO_PROFILE_DIR="${profile_dir}"
    )

    echo "Start building ..."
    case "$1" in
        Debug)
            cmake .. -DCMAKE_BUILD_TYPE=Debug "${cmake_common_options[@]}" $cmake_gen
            ;;
        DebugShared)
            cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_DROGON_SHARED=ON -DCMAKE_CXX_VISIBILITY_PRESET=hidden -DCMAKE_VISIBILITY_INLINES_HIDDEN=1 "${cmake_common_options[@]}" $cmake_gen
            ;;
        Release|*)
            cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-16 "${cmake_common_options[@]}" $cmake_gen
            ;;
    esac

    #If errors then exit
    # shellcheck disable=SC2181
    if [ "$?" != "0" ]; then
        # shellcheck disable=SC2242
        exit -1
    fi

    $make_program $make_flags

    #If errors then exit
    # shellcheck disable=SC2181
    if [ "$?" != "0" ]; then
        # shellcheck disable=SC2242
        exit -1
    fi

    #echo "Installing ..."
    #sudo $make_program install

    #Go back to the current directory
    cd $current_dir || exit

    if [[ "$run_after_build" == "ON" ]]; then
        echo "Starting ..."
        $build_dir/loong-boot
    else
        echo "Build done: ${build_dir}/loong-boot"
    fi
    #Ok!
}

# shellcheck disable=SC2209
make_program=make
make_flags=''
cmake_gen=''
parallel=1

case $(uname) in
 FreeBSD)
    nproc=$(sysctl -n hw.ncpu)
    ;;
 Darwin)
    nproc=$(sysctl -n hw.ncpu) # sysctl -n hw.ncpu is the equivalent to nproc on macOS.
    ;;
 *)
    nproc=$(nproc)
    ;;
esac

# simulate ninja's parallelism
# shellcheck disable=SC2194
case "$nproc" in
 1)
    parallel=$(( nproc + 1 ))
    ;;
 2)
    parallel=$(( nproc + 1 ))
    ;;
 *)
    parallel=$(( nproc + 2 ))
    ;;
esac

if command -v ninja >/dev/null 2>&1; then
    make_program=ninja
    cmake_gen='-GNinja'
else
    make_flags="$make_flags -j$parallel"
fi

# Parse extra arguments for PGO
for arg in "$@"; do
    case "$arg" in
        --no-run)
            run_after_build=OFF
            ;;
        gen)
            gen_pgo=ON
            ;;
        use)
            use_pgo=ON
            ;;
    esac
done

# 默认构建类型改为 Debug
build_type="Debug"
if [[ "$1" == "-t" ]]; then
    build_type="Debug"
    shift
elif [[ "$1" == "-tshared" ]]; then
    build_type="DebugShared"
    shift
elif [[ "$1" == "-trelease" ]]; then
    build_type="Release"
    build_dir='./build-release'
    shift
elif [[ "$1" == "-tperf" ]]; then
    build_type="Release"
    aggressive_opt=ON
    build_dir='./build-perf'
    shift
elif [[ "$1" == "-tlto" ]]; then
    build_type="Release"
    aggressive_opt=ON
    use_lto=ON
    build_dir='./build-perf-lto'
    shift
elif [[ "$1" == "-tpgo-gen" ]]; then
    build_type="Release"
    aggressive_opt=ON
    gen_pgo=ON
    build_dir='./build-pgo-gen'
    profile_dir="${PWD}/build-pgo-gen/pgo-data"
    shift
elif [[ "$1" == "-tpgo-use" ]]; then
    build_type="Release"
    aggressive_opt=ON
    use_lto=ON
    use_pgo=ON
    build_dir='./build-pgo-use'
    profile_dir="${PWD}/build-pgo-gen/pgo-data"
    shift
fi

if [[ "$*" == *"--no-native"* ]]; then
    use_native=OFF
fi
if [[ "$*" == *"--fast-linker"* ]]; then
    use_fast_linker=ON
fi

if [[ "$gen_pgo" == "ON" && "$use_pgo" == "ON" ]]; then
    echo "错误：PGO gen 和 use 不能同时开启"
    exit 1
fi

build_loong "$build_type" "$@"
