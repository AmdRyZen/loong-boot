#include "aop/Application.h"
#include <drogon/drogon.h>

int main()
{
    try
    {
        // 加载配置
        app().loadConfigFile("config.json");

        // 注册 Filter 插件
        app().enableGzip(true).enableBrotli(true).enableSendfile(true);

        // 初始化服务 如kfk等
        App::Application();

        if (const unsigned int cpu_cores = std::thread::hardware_concurrency() * 2; cpu_cores != 0)
            app().setThreadNum(cpu_cores);

        // 启动项目
        app().run();

    } catch (const std::exception& e)
    {
        std::cout << "Application: err  " << e.what() << std::endl;
    }
    return 0;
}
