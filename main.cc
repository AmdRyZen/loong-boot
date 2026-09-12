#include "aop/Application.h"
#include <drogon/drogon.h>
#include <filesystem>
#include <cstdlib>

namespace {
std::string resolveConfigFile()
{
    // 优先级 1: 环境变量 LOONG_PROFILE (dev/prod/test)
    if (const char* env_profile = std::getenv("LOONG_PROFILE"))
    {
        std::string profileFile = std::string("config-") + env_profile + ".json";
        if (std::filesystem::exists(profileFile))
        {
            LOG_INFO << "Loading config from profile environment: " << profileFile;
            return profileFile;
        }
    }
    // 优先级 2: 默认 config.json
    if (std::filesystem::exists("config.json"))
    {
        return "config.json";
    }
    // 优先级 3: 研发环境默认兜底
    if (std::filesystem::exists("config-dev.json"))
    {
        return "config-dev.json";
    }
    return "config.json";
}
}

int main(int argc, char* argv[])
{
    try
    {
        // 允许通过命令行传参指定配置文件，如 ./loong-boot config-prod.json
        const std::string configFile = argc > 1 && argv[1] != nullptr ? argv[1] : resolveConfigFile();
        LOG_INFO << "Active configuration: " << configFile;

        // 加载配置
        drogon::app().loadConfigFile(configFile);

        // 注册 Filter 插件
        drogon::app().enableGzip(true).enableBrotli(true).enableSendfile(true);

        // 初始化服务 如kfk等
        App::Application();

        // IO 线程数（= drogon 事件循环个数）。
        //
        // ⚠️ 这一行在 loadConfigFile 之后执行，**会覆盖 config.json 里的
        // number_of_threads** —— 只看配置文件会以为线程数是 8，实际是 核数×2。
        // 排查任何与线程/事件循环相关的问题（扇出分组、定时器、连接归属）前，
        // 必须先意识到这一点。
        //
        // 线程数同时决定扇出的跨线程唤醒上界：房间订阅者分散在 K 个 loop 上时，
        // 每条消息最多需要 K 次唤醒（见 RoomRegistry 的 LoopH 分组）。
        // LOONG_IO_THREADS 可显式覆盖，用于压测不同线程数或在异构核机器上收敛。
        unsigned int ioThreads = std::thread::hardware_concurrency() * 2;
        if (const char* env = std::getenv("LOONG_IO_THREADS"); env != nullptr && *env != '\0')
        {
            try
            {
                const unsigned long v = std::stoul(env);
                if (v > 0 && v <= 4096)
                {
                    ioThreads = static_cast<unsigned int>(v);
                }
                else
                {
                    LOG_WARN << "LOONG_IO_THREADS 取值非法（需 1..4096），忽略: " << env;
                }
            }
            catch (const std::exception&)
            {
                LOG_WARN << "LOONG_IO_THREADS 不是合法数字，忽略: " << env;
            }
        }
        if (ioThreads != 0)
        {
            drogon::app().setThreadNum(ioThreads);
        }
        LOG_INFO << "IO threads: " << ioThreads;

        // 启动项目
        drogon::app().run();

    } catch (const std::exception& e)
    {
        std::cout << "Application: err  " << e.what() << std::endl;
        // 启动/运行失败必须以非零码退出，否则 K8s、systemd、CI 都看不到失败
        return 1;
    }
    return 0;
}
