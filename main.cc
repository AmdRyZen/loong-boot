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
            LOG_DEBUG << "Loading config from profile environment: " << profileFile;
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
        LOG_DEBUG << "Active configuration: " << configFile;

        // 加载配置
        drogon::app().loadConfigFile(configFile);

        // 注册 Filter 插件
        drogon::app().enableGzip(true).enableBrotli(true).enableSendfile(true);

        // 初始化服务 如kfk等
        App::Application();

        // IO 线程数（= drogon 事件循环个数）优先级：
        //   1) 环境变量 LOONG_IO_THREADS —— 显式覆盖（压测、异构核机器收敛）
        //   2) 配置文件里显式指定的 number_of_threads / threads_num
        //   3) 兜底 hardware_concurrency() * 2
        //
        // ⚠️ 这里修掉了一个历史坑：旧代码无条件 setThreadNum(hardware_concurrency()*2)，
        // 把配置文件里显式写好的 number_of_threads 静默覆盖掉 —— 配置里的值从来没生效过，
        // 且不打印任何日志，排查线程/事件循环相关问题时极易误判（扇出分组、定时器、
        // 连接归属都依赖线程数）。
        //
        // 注意 drogon 的 ConfigLoader 在 number_of_threads 缺省时会把它设为 1，
        // 所以「配置值 > 1」才视为显式指定；否则走兜底，避免退化成单线程。
        unsigned int ioThreads = 0;
        const char* ioThreadsSource = "default(cores*2)";

        // loadConfigFile 已经把配置值应用进去了，这里取回来判断
        if (const size_t fromConfig = drogon::app().getThreadNum(); fromConfig > 1)
        {
            ioThreads = static_cast<unsigned int>(fromConfig);
            ioThreadsSource = "config";
        }

        if (const char* env = std::getenv("LOONG_IO_THREADS"); env != nullptr && *env != '\0')
        {
            try
            {
                const unsigned long v = std::stoul(env);
                if (v > 0 && v <= 4096)
                {
                    ioThreads = static_cast<unsigned int>(v);
                    ioThreadsSource = "env";
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

        if (ioThreads == 0)
        {
            ioThreads = std::thread::hardware_concurrency() * 2;
            if (ioThreads == 0)
            {
                ioThreads = 1; // hardware_concurrency() 可能返回 0
            }
        }

        drogon::app().setThreadNum(ioThreads);

        // 同时写 stdout 与日志：stdout 不受 log_level 影响（WARN 级别下 LOG_INFO
        // 是不落盘的），确保这个值在任何配置下都看得见。
        LOG_DEBUG << "IO threads: " << ioThreads << " (source: " << ioThreadsSource << ")";
        std::cout << "IO threads: " << ioThreads << " (source: " << ioThreadsSource << ")"
                  << std::endl;

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
