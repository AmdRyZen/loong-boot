#include "aop/Application.h"
#include <drogon/drogon.h>

int main()
{
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

    } catch (const std::exception& e)
    {
        std::cout << "Application: err  " << e.what() << std::endl;
    }
    return 0;
}
