# 性能构建与压测指南

这个项目当前偏向本机极限性能测试，CMake 已经支持 native 指令集、激进优化、LTO 和 PGO。

## 构建档位

如果只想编译，不想编译后立刻启动服务，加 `--no-run`。

```bash
# Debug：用于调试，Debug 配置里带 leak sanitizer
./build.sh -t --no-run

# Release 基准档：使用 CMake Release 默认优化
./build.sh -trelease --no-run

# 性能档：Release + AGGRESSIVE_OPT
./build.sh -tperf --no-run

# LTO 性能档：Release + AGGRESSIVE_OPT + LTO
./build.sh -tlto --no-run
```

对应产物：

```text
./build/loong-boot
./build-release/loong-boot
./build-perf/loong-boot
./build-perf-lto/loong-boot
```

## PGO 流程

PGO 分两步：先构建“采样版”，用真实流量跑一遍生成 profile；再构建“使用 profile 的优化版”。

```bash
# 1. 构建 PGO 采样版
./build.sh -tpgo-gen --no-run

# 2. 启动采样版服务
./build-pgo-gen/loong-boot

# 3. 另开终端，跑一轮代表真实业务的压测
wrk -t8 -c256 -d60s http://127.0.0.1:9090/test

# 4. 压测结束后，停止服务

# 5. 构建 PGO + LTO 最终优化版
./build.sh -tpgo-use --no-run

# 6. 启动最终优化版再压测
./build-pgo-use/loong-boot
```

PGO profile 数据目录：

```text
./build-pgo-gen/pgo-data
```

## 推荐压测矩阵

建议同一台机器、同一个接口、同一组参数分别跑下面几档：

```text
1. Release 基准档
2. Release + AGGRESSIVE_OPT
3. Release + AGGRESSIVE_OPT + LTO
4. Release + AGGRESSIVE_OPT + PGO + LTO
```

每组记录：

```text
QPS / RPS
p50
p95
p99
CPU 使用率
内存占用
错误率
```

示例：

```bash
./build.sh -trelease --no-run
./build-release/loong-boot
wrk -t8 -c256 -d60s http://127.0.0.1:9090/test

./build.sh -tperf --no-run
./build-perf/loong-boot
wrk -t8 -c256 -d60s http://127.0.0.1:9090/test

./build.sh -tlto --no-run
./build-perf-lto/loong-boot
wrk -t8 -c256 -d60s http://127.0.0.1:9090/test
```

## 常用参数

关闭 native 指令集优化，方便测试可移植构建：

```bash
./build.sh -tlto --no-native --no-run
```

尝试使用高速链接器。如果机器上有 `mold` 或 `lld`，CMake 会优先使用：

```bash
./build.sh -tlto --fast-linker --no-run
```

只编译不启动：

```bash
./build.sh -tperf --no-run
```

编译后自动启动：

```bash
./build.sh -tperf
```

## 原始 CMake 命令

不用 `build.sh`，也可以直接跑 CMake：

```bash
cmake -S . -B cmake-build-perf-lto \
  -DCMAKE_BUILD_TYPE=Release \
  -DAGGRESSIVE_OPT=ON \
  -DUSE_LTO=ON \
  -DUSE_NATIVE_ARCH=ON

cmake --build cmake-build-perf-lto -j 8
```

PGO 采样版：

```bash
cmake -S . -B cmake-build-pgo-gen \
  -DCMAKE_BUILD_TYPE=Release \
  -DAGGRESSIVE_OPT=ON \
  -DUSE_PGO_GEN=ON \
  -DPGO_PROFILE_DIR="$(pwd)/cmake-build-pgo-gen/pgo-data"

cmake --build cmake-build-pgo-gen -j 8
```

PGO 使用版：

```bash
cmake -S . -B cmake-build-pgo-use \
  -DCMAKE_BUILD_TYPE=Release \
  -DAGGRESSIVE_OPT=ON \
  -DUSE_LTO=ON \
  -DUSE_PGO_USE=ON \
  -DPGO_PROFILE_DIR="$(pwd)/cmake-build-pgo-gen/pgo-data"

cmake --build cmake-build-pgo-use -j 8
```

## 注意事项

- `Release` 本身已经用了比较激进的基础优化。
- `AGGRESSIVE_OPT=ON` 不一定每个接口都更快，必须用压测结果判断。
- `USE_LTO=ON` 可能提升跨文件内联和去虚化，但构建和链接会更慢。
- `PGO` 的效果强依赖采样流量。采样接口太单一，可能会让其他接口变慢。
- 每轮压测前最好预热几秒，避免冷启动影响结果。
- 对比不同构建档位时，保持线程数、连接数、接口、请求体、压测时长完全一致。
