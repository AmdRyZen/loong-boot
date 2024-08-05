#include "aop/Application.h"
#include <drogon/drogon.h>
#include <csignal> // 包含 signal 处理头文件

std::atomic<bool> stopRequested(false);

void signalHandler(int signal) {
    stopRequested.store(true, std::memory_order_relaxed);
}

int main()
{
    // 设置信号处理函数
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    try
    {
        // 注册 GzipFilter 插件
        app().enableGzip(true);

        // 加载配置
        app().loadConfigFile("config.json");

        // 初始化服务 如kfk等
        App::Application();

        // 启动项目
        app().run();

        // 运行主循环，检查停止标志
        while (!stopRequested.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } catch (const std::exception& e)
    {
        std::cout << "Application: err  " << e.what() << std::endl;
    }
    return 0;
}
