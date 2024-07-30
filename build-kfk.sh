STATIC_LIB_libzstd=$(brew ls -v zstd | grep libzstd.a$) ./configure --enable-static
./configure --install-deps
make -j8
sudo make install


# brew services start zookeeper
# brew services restart kafka