STATIC_LIB_libzstd=$(brew ls -v zstd | grep libzstd.a$) ./configure --enable-static
./configure --install-deps
make -j8
sudo make install


# brew services start zookeeper
# brew services restart kafka



#paho-mqttpp3
#cmake -DPAHO_WITH_SSL=ON .. -DCMAKE_CXX_COMPILER=g++-14