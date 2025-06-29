#cmake - DCMAKE_BUILD_TYPE = Release - B./ build && cmake-- build build - j4-- config Release

function build_loong()
{
    #clang - format - style = file - i controllers/*

    #Update the submodule and initialize
    #git submodule update --init

    #Save current directory
    current_dir="${PWD}"

    #The folder in which we will build
    build_dir='./build'
    if [ -d $build_dir ]; then
        echo "Deleted folder: ${build_dir}"
        rm -rf $build_dir
    fi

    #Create building folder
    echo "Created building folder: ${build_dir}"
    mkdir $build_dir

    echo "Entering folder: ${build_dir}"
    cd $build_dir || exit

    echo "Start building ..."
    case "$1" in
        Debug)
            cmake .. -DCMAKE_BUILD_TYPE=Debug $cmake_gen
            ;;
        DebugShared)
            cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_DROGON_SHARED=ON -DCMAKE_CXX_VISIBILITY_PRESET=hidden -DCMAKE_VISIBILITY_INLINES_HIDDEN=1 $cmake_gen
            ;;
        Release|*)
            cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-15 $cmake_gen
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

if [ "$1" = "-t" ]; then
    build_loong Debug
elif [ "$1" = "-tshared" ]; then
    build_loong DebugShared
else
    build_loong Release
fi