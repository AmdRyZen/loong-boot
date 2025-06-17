#protoc -I=./ --cpp_out=./ ./user.proto

# ./configure CXX=/opt/homebrew/Cellar/gcc/14.1.0_1/bin/g++-14
# cmake -DCMAKE_BUILD_TYPE=Release
# make -j8 CXX=/opt/homebrew/Cellar/gcc/14.1.0_1/bin/g++-14 CFLAGS=-Ofast

#g++ -std=c++20 main.cpp game.pb.cc `pkg-config --cflags --libs protobuf` -lpthread


sudo cp bazel-bin/src/google/protobuf/libprotobuf.a /usr/local/lib/

git checkout v3.21.12
./autogen.sh
./configure --prefix=/usr/local
make -j$(sysctl -n hw.logicalcpu)
sudo make install