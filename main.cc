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

        if (const unsigned int cpu_cores = std::thread::hardware_concurrency() * 2; cpu_cores != 0)
            drogon::app().setThreadNum(cpu_cores);

        // 启动项目
        drogon::app().run();

    } catch (const std::exception& e)
    {
        std::cout << "Application: err  " << e.what() << std::endl;
    }
    return 0;
}
