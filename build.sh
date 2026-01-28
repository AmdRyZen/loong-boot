#cmake - DCMAKE_BUILD_TYPE = Release - B./ build && cmake-- build build - j4-- config Release

# PGO options
use_pgo=OFF
gen_pgo=OFF

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

    #The folder in which we will build
    if [[ "$gen_pgo" == "ON" ]]; then
        build_dir='./build-pgo-gen'
    else
        build_dir='./build'
    fi
    # 只有在 gen 阶段时，才清除 build 目录
    if [[ "$gen_pgo" == "ON" && -d $build_dir ]]; then
        echo "Deleted folder: ${build_dir}"
        rm -rf $build_dir
    fi

    #Create building folder
    echo "Created building folder: ${build_dir}"
    mkdir -p $build_dir

    echo "Entering folder: ${build_dir}"
    cd $build_dir || exit

    if [[ "$use_pgo" == "ON" ]]; then
        echo "复制 .gcda 文件用于 PGO..."
        gen_dir="../build-pgo-gen"
        use_dir="."
        if [ -d "$gen_dir" ]; then
            find "$gen_dir" -name "*.gcda" | while read -r src; do
                dst="${src/$gen_dir/$use_dir}"
                dst_dir=$(dirname "$dst")
                mkdir -p "$dst_dir"
                cp "$src" "$dst"
            done
        else
            echo "警告：未找到 PGO 生成目录 $gen_dir"
        fi
    fi

    echo "Start building ..."
    case "$1" in
        Debug)
            cmake .. -DCMAKE_BUILD_TYPE=Debug -DUSE_PGO_GEN=${gen_pgo} -DUSE_PGO_USE=${use_pgo} $cmake_gen
            ;;
        DebugShared)
            cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_DROGON_SHARED=ON -DCMAKE_CXX_VISIBILITY_PRESET=hidden -DCMAKE_VISIBILITY_INLINES_HIDDEN=1 -DUSE_PGO_GEN=${gen_pgo} -DUSE_PGO_USE=${use_pgo} $cmake_gen
            ;;
        Release|*)
            cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-15 -DUSE_PGO_GEN=${gen_pgo} -DUSE_PGO_USE=${use_pgo} $cmake_gen
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

    echo "Starting ..."
    $build_dir/loong-boot
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
case nproc in
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

if [ -f /bin/ninja ]; then
    make_program=ninja
    cmake_gen='-GNinja'
else
    make_flags="$make_flags -j$parallel"
fi

# Parse extra arguments for PGO
for arg in "$@"; do
    case "$arg" in
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
    shift
fi

# 检查 gen/use 同时出现且类型不一致
if [[ "$*" == *gen* && "$*" == *use* ]]; then
    if [[ "$build_type" != "Release" ]]; then
        echo "警告：gen 和 use 同时指定时，建议构建类型保持一致（当前为 $build_type，建议为 Release）"
    fi
fi

# ./build.sh gen 或 ./build.sh use 时，默认使用 Release 保持一致
if [[ "$*" == *gen* || "$*" == *use* ]]; then
    build_type="Release"
fi

build_loong "$build_type" "$@"