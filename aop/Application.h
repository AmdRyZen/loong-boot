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
#include "kafka/KafkaManager.h"
#include "kafkaManager/AsyncKafkaConsumer.h"
#include "kafkaManager/AsyncKafkaConsumerOne.h"
#include <tbb/global_control.h>
#include "utils/TraceContext.h"
#include "utils/PrometheusMetrics.h"

inline TrieService trieService;
static tbb::global_control tbb_limit(tbb::global_control::max_allowed_parallelism, std::thread::hardware_concurrency()); // 限制最大线程数

namespace App {
constexpr char Loong[] = "\n"
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
    std::cout << Loong << std::endl;
    std::cout << "A utility for drogon" << std::endl;
    std::cout << std::format("Version: {}", DROGON_VERSION) << std::endl;
    std::cout << std::format("Git commit: {}", DROGON_VERSION_SHA1) << std::endl;
    std::cout << std::format("Ssl/tls backend: {}",  trantor::utils::tlsBackend()) << std::endl;
    std::cout << std::endl;

    try
    {
        //TbbCoroutinePool::instance().init();

        // 获取 KafkaManager 的配置
        const std::string brokers = drogon::app().getCustomConfig()["kafka_manager"]["bootstrap.servers"].asString();

        // 初始化 KafkaManager
        kafka::KafkaManager::instance().initialize(brokers);

        // 创建一个消费者实例
        // ✅ 正确创建 AsyncKafkaConsumer
        static AsyncKafkaConsumer asyncKafkaConsumer(
            {"message_topic"},  // topic list
            [](const std::string &msg) -> drogon::Task<> {
                //LOG_INFO << "message_topic msg: " << msg;
                co_return;
            },
            4 // 可调线程数
        );

        static AsyncKafkaConsumerOne asyncKafkaConsumerOne(
            {"message_topic_one"},  // topic list
            [](const std::string &msg) -> drogon::Task<> {
                //LOG_INFO << "message_topic_one msg: " << msg;
                co_return;
            },
            4 // 可调线程数
        );

        // 初始化 MqttManager 并连接到 MQTT broker  mosquitto/emqx start
        //MqttManager::instance().initialize(app().getCustomConfig()["mqtt_manager"]["servers"].asString(), app().getCustomConfig()["mqtt_manager"]["client_id"].asString());

        // 创建并启动消费者实例
        //static MqttConsumer mqttConsumer;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "initialization failed: " << e.what();
    }

    drogon::app().registerBeginningAdvice([]() {
        std::string word_path;
        std::string stopped_path;
        word_path.append(std::filesystem::current_path()).append("/public/word.txt");
        stopped_path.append(std::filesystem::current_path()).append("/public/stopped.txt");
        TrieService::loadFromFile(word_path);
        TrieService::loadStopWordFromFile(stopped_path);
        LOG_DEBUG << "TrieService load is success!";
        std::cout << std::endl;
    });

    // 全局路由注册：/metrics 监控端点直接代码级注册，避免反射加载顺序失效
    drogon::app().registerHandler(
        "/metrics",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            const auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeString("text/plain; version=0.0.4; charset=utf-8");
            resp->setBody(Metrics::PrometheusRegistry::instance().exportPrometheusText());
            // ⚠️ 这里必须用负数。drogon 的 setExpiredTime 语义与直觉相反：
            //    0 = **永久缓存**，负数 = 不缓存，默认 -1。
            // 之前写的 0 会让每个 IO 线程把「它服务的第一个 /metrics 请求」的
            // 快照永久冻结（缓存是 IOThreadStorage，每线程一份），之后该线程上
            // 的所有抓取都返回旧值 —— 监控静默失真，排查时极易被误导。
            resp->setExpiredTime(-1);
            callback(resp);
        },
        {drogon::Get});

    // 全局分布式链路追踪 TraceId 拦截与注入
    drogon::app().registerPreRoutingAdvice([](const drogon::HttpRequestPtr& req,
                                              drogon::AdviceCallback&& acb,
                                              drogon::AdviceChainCallback&& accb) {
        const std::string traceId = Trace::TraceContext::getOrCreateTraceId(req);
        // 将 TraceId 存入请求上下文，供全链路 Controller/Service 使用
        req->attributes()->insert("trace_id", traceId);
        // 记录请求进入时间用于度量
        const auto nowMs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        req->attributes()->insert("req_start_us", nowMs);

        accb();
    });

    // 全局响应拦截：统一自动回填 X-Trace-Id 响应头，并记录 Prometheus 请求指标
    drogon::app().registerPostHandlingAdvice([](const drogon::HttpRequestPtr& req,
                                                const drogon::HttpResponsePtr& resp) {
        if (!req || !resp) return;

        // 回填 TraceId
        if (req->attributes()->find("trace_id")) {
            const auto traceId = req->attributes()->get<std::string>("trace_id");
            resp->addHeader(std::string(Trace::TRACE_HEADER), traceId);
        }

        // 收集 Prometheus 统计指标
        if (req->attributes()->find("req_start_us")) {
            const auto startUs = req->attributes()->get<int64_t>("req_start_us");
            const auto endUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const double durationMs = static_cast<double>(endUs - startUs) / 1000.0;
            Metrics::PrometheusRegistry::instance().recordHttpRequest(
                std::string(req->methodString()), resp->statusCode(), durationMs);
        }
    });
}
}  // namespace App


#endif  //LEARNING_CPP_APPLICATION_H
