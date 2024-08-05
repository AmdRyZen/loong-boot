//
// Created by 神圣·凯莎 on 2022/5/12.
//

#ifndef LEARNING_CPP_APPLICATION_H
#define LEARNING_CPP_APPLICATION_H

#include "service/TrieService.h"
#include "threadPool/threadPool.h"
#include <drogon/drogon.h>
#include <filesystem>
#include <iostream>
#include <drogon/version.h>
#include <trantor/utils/Utilities.h>
#include <boost/format.hpp>
#include <boost/date_time.hpp>
#include "../kafkaManager/kafkaManager.h"
#include "kafkaManager/AsyncKafkaConsumer.h"

inline TrieService trieService;

inline ThreadPool pool(2);

namespace App {
constexpr char drogon[] = "\n"
                      "                       .::::.\n"
                      "                     .::::::::.\n"
                      "                    :::::::::::  HELLO LOONG\n"
                      "                ..:::::::::::'\n"
                      "              '::::::::::::'\n"
                      "                .::::::::::\n"
                      "           '::::::::::::::..\n"
                      "                ..::::::::::::.\n"
                      "              ``::::::::::::::::\n"
                      "               ::::``:::::::::'        .:::.\n"
                      "              ::::'   ':::::'       .::::::::.\n"
                      "            .::::'      ::::     .:::::::'::::.\n"
                      "           .:::'       :::::  .:::::::::' ':::::.\n"
                      "          .::'        :::::.:::::::::'      ':::::.\n"
                      "         .::'         ::::::::::::::'         ``::::.\n"
                      "     ...:::           ::::::::::::'              ``::.\n"
                      "    ````':.          ':::::::::'                  ::::..\n"
                      "                       '.:::::'                    ':'````..\n"
                      "\n";
/*std::string drogon = "    ┌───┐   ┌───┬───┬───┬───┐ ┌───┬───┬───┬───┐ ┌───┬───┬───┬───┐ ┌───┬───┬───┐\n"
                     "    │Esc│   │ F1│ F2│ F3│ F4│ │ F5│ F6│ F7│ F8│ │ F9│F10│F11│F12│ │P/S│S L│P/B│  ┌┐    ┌┐    ┌┐\n"
                     "    └───┘   └───┴───┴───┴───┘ └───┴───┴───┴───┘ └───┴───┴───┴───┘ └───┴───┴───┘  └┘    └┘    └┘\n"
                     "    ┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───────┐ ┌───┬───┬───┐ ┌───┬───┬───┬───┐\n"
                     "    │~ `│! 1│@ 2│# 3│$ 4│% 5│^ 6│& 7│* 8│( 9│) 0│_ -│+ =│ BacSp │ │Ins│Hom│PUp│ │N L│ / │ * │ - │\n"
                     "    ├───┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─────┤ ├───┼───┼───┤ ├───┼───┼───┼───┤\n"
                     "    │ Tab │ Q │ W │ E │ R │ T │ Y │ U │ I │ O │ P │{ [│} ]│ | \\ │ │Del│End│PDn│ │ 7 │ 8 │ 9 │   │\n"
                     "    ├─────┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴─────┤ └───┴───┴───┘ ├───┼───┼───┤ + │\n"
                     "    │ Caps │ A │ S │ D │ F │ G │ H │ J │ K │ L │: ;│\" '│ Enter  │               │ 4 │ 5 │ 6 │   │\n"
                     "    ├──────┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴────────┤     ┌───┐     ├───┼───┼───┼───┤\n"
                     "    │ Shift  │ Z │ X │ C │ V │ B │ N │ M │< ,│> .│? /│  Shift   │     │ ↑ │     │ 1 │ 2 │ 3 │   │\n"
                     "    ├─────┬──┴─┬─┴──┬┴───┴───┴───┴───┴───┴──┬┴───┼───┴┬────┬────┤ ┌───┼───┼───┐ ├───┴───┼───┤ E││\n"
                     "    │ Ctrl│    │Alt │         Space         │ Alt│    │    │Ctrl│ │ ← │ ↓ │ → │ │   0   │ . │←─┘│\n"
                     "    └─────┴────┴────┴───────────────────────┴────┴────┴────┴────┘ └───┴───┴───┘ └───────┴───┴───┘";*/
/*std::string drogon = "´´´´´´´´██´´´´´´´\n"
                     "´´´´´´´████´´´´´´\n"
                     "´´´´´████████´´´´\n"
                     "´´`´███▒▒▒▒███´´´´´\n"
                     "´´´███▒●▒▒●▒██´´´\n"
                     "´´´███▒▒▒▒▒▒██´´´´´\n"
                     "´´´███▒▒▒▒██´                      \n"
                     "´´██████▒▒███´´´´´                 \n"
                     "´██████▒▒▒▒███´´                  \n"
                     "██████▒▒▒▒▒▒███´´´´                \n"
                     "´´▓▓▓▓▓▓▓▓▓▓▓▓▓▒´´                 \n"
                     "´´▒▒▒▒▓▓▓▓▓▓▓▓▓▒´´´´´              \n"
                     "´.▒▒▒´´▓▓▓▓▓▓▓▓▒´´´´´              \n"
                     "´.▒▒´´´´▓▓▓▓▓▓▓▒                   \n"
                     "..▒▒.´´´´▓▓▓▓▓▓▓▒                  \n"
                     "´▒▒▒▒▒▒▒▒▒▒▒▒                      \n"
                     "´´´´´´´´´███████´´´´´              \n"
                     "´´´´´´´´████████´´´´´´´\n"
                     "´´´´´´´█████████´´´´´´\n"
                     "´´´´´´██████████´´´´             \n"
                     "´´´´´´██████████´´´                    \n"
                     "´´´´´´´█████████´´\n"
                     "´´´´´´´█████████´´´\n"
                     "´´´´´´´´████████´´´´´\n"
                     "________▒▒▒▒▒\n"
                     "_________▒▒▒▒\n"
                     "_________▒▒▒▒\n"
                     "________▒▒_▒▒\n"
                     "_______▒▒__▒▒\n"
                     "_____ ▒▒___▒▒\n"
                     "_____▒▒___▒▒\n"
                     "____▒▒____▒▒\n"
                     "___▒▒_____▒▒\n"
                     "███____ ▒▒\n"
                     "████____███\n"
                     "█ _███_ _█_███";*/

class Application final
{
  public:
    [[gnu::always_inline]] inline Application();

    ~Application() = default;

    Application initialization();
};

Application::Application()
{
    // 创建一个定时器，每隔10秒执行一次
    /*drogon::HttpAppFramework::instance().getLoop()->runEvery(10.0, [] {
        std::cout << "定时器触发了" << std::endl;
    });*/

    try
    {
        // 获取 KafkaManager 的配置
        const std::string brokers = app().getCustomConfig()["kafka_manager"]["bootstrap.servers"].asString();

        // 初始化 KafkaManager
        KafkaManager::instance().initialize(brokers);

        // 创建一个消费者实例
        static AsyncKafkaConsumer kafkaConsumer;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "Kafka initialization failed: " << e.what();
    }

    app().registerBeginningAdvice([]() {
        std::cout << drogon << std::endl;
        std::cout << "A utility for drogon" << std::endl;
        std::cout << std::format("Version: {}", DROGON_VERSION) << std::endl;
        std::cout << std::format("Git commit: {}", DROGON_VERSION_SHA1) << std::endl;
        std::cout << std::format("Ssl/tls backend: {}",  trantor::utils::tlsBackend()) << std::endl;

        std::string word_path;
        std::string stopped_path;
        word_path.append(std::filesystem::current_path()).append("/public/word.txt");
        stopped_path.append(std::filesystem::current_path()).append("/public/stopped.txt");
        TrieService::loadFromFile(word_path);
        TrieService::loadStopWordFromFile(stopped_path);
        std::cout << "TrieService load is success!" << std::endl;

        boost::posix_time::ptime current_datetime = boost::posix_time::second_clock::local_time();
        std::cout << "Current date and time: " << current_datetime << std::endl;
        std::cout << std::endl << std::endl;
    });

    app().registerPreRoutingAdvice([](const drogon::HttpRequestPtr& req,
                                              drogon::AdviceCallback&& acb,
                                              drogon::AdviceChainCallback&& accb) {
        // todo ...
        //std::cout << "preRouting1!" << std::endl;
        accb();
    });
}
}  // namespace App


#endif  //LEARNING_CPP_APPLICATION_H
