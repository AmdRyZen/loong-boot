STATIC_LIB_libzstd=$(brew ls -v zstd | grep libzstd.a$) ./configure --enable-static
./configure --install-deps
make -j8
sudo make install


# brew services start zookeeper
# brew services restart kafka
/opt/homebrew/Cellar/kafka/4.0.0/libexec/bin/kafka-topics.sh --bootstrap-server localhost:9092 --delete --topic  message_topic
/opt/homebrew/Cellar/kafka/4.0.0/libexec/bin/kafka-topics.sh --bootstrap-server localhost:9092 --list

sudo rm -rf /usr/local/lib/libkafka-core.a
sudo rm -rf /usr/local/include/kafka
sudo rm -rf /usr/local/lib/cmake/KafkaCore


#paho-mqttpp3
#cmake -DPAHO_WITH_SSL=ON .. -DCMAKE_CXX_COMPILER=g++-15

# 构建 hpx  在同一台 Mac 上启动多个 HPX 节点并通过 hpx::async 跨节点通信的完整 demo，
cmake .. -DCMAKE_BUILD_TYPE=Release -DHPX_WITH_MALLOC=mimalloc -DHPX_WITH_FETCH_ASIO=ON

# 第一个节点（主节点）
./build/loong-boot --hpx:locality=0 --hpx:ini=hpx.os_threads=2 --hpx:agas=localhost:7910 --hpx:run_hpx_main=yes

# 第二个节点
./build/loong-boot --hpx:locality=1 --hpx:ini=hpx.os_threads=2 --hpx:agas=localhost:7910 --hpx:run_hpx_main=no

# 第三个节点
./build/loong-boot --hpx:locality=2 --hpx:ini=hpx.os_threads=2 --hpx:agas=localhost:7910 --hpx:run_hpx_main=no