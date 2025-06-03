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