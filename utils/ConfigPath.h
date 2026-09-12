#pragma once
#include <string>

namespace Config {

// 当前进程【实际加载的那个】配置文件路径（由 main.cc 在 loadConfigFile 之前写入）。
//
// 为什么需要这么个东西：
//   drogon::app().getCustomConfig() 返回的是**启动时解析出来的内存副本**，
//   进程跑起来之后永远不会重读文件。所以想实现「改配置不重启就生效」的开关热更新，
//   只盯着 getCustomConfig() 是白费力气 —— 必须知道文件在哪，自己再读一遍。
//
//   （这一点是实测出来的：按 getCustomConfig() 实现的热更新，改完配置文件后
//     观察用的 gauge 一直不动，排查才发现读的是内存副本。）
//
// 为什么不在 ChatWebsocket 里重新推导一遍路径：main.cc 的解析逻辑有三级优先级
// （LOONG_PROFILE 环境变量 → config.json → config-dev.json）且还接受命令行参数，
// 重新推导很可能得到与真正加载的那个不同的文件 —— 那比读内存副本更危险。
inline std::string& filePath()
{
    static std::string path = "config.json";
    return path;
}

} // namespace Config
