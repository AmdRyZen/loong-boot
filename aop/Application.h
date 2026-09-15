//
// Created by 神圣·凯莎 on 2022/5/12.
//

#ifndef LEARNING_CPP_APPLICATION_H
#define LEARNING_CPP_APPLICATION_H

#include "service/TrieService.h"
#include "threadPool/threadPool.h"
#include <drogon/drogon.h>
#include <cstdio>
#include <cstdlib>
#include <functional>
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

// ── 「进程退出前：先停 Kafka 消费者、再排空 TBB」的钩子 ──────────────────────
//
// 为什么需要它：
//   trantor 的 Socket::bind() 失败时是 `LOG_SYSERR << ...; exit(1);`
//   （trantor/net/inner/Socket.cc:67）—— 端口被占就会走到这里。
//   而 exit() 只跑静态析构，【不等其他线程】。此时 TBB worker 可能仍在
//   AsyncKafkaConsumer::submitMessageTask 里打 LOG_*，但
//   Logger::outputFunc_()/flushFunc_() 这两个【函数内静态】已经析构，
//   于是 ~Logger() → AsyncFileLogger::flush() 在已死的 mutex 上加锁
//   ⇒ std::system_error(EINVAL) ⇒ 从析构里抛出 ⇒ terminate ⇒ abort(134)。
//
// 所以退出路径上必须先 requestStop() 两个消费者（join 掉 poll 线程）、
// 再 waitAll() 排空 TBB，然后才轮到 logger 静态析构。
//
// 钩子体与 std::atexit 的注册都在 Application() 构造函数里完成，
// 但【顺序很关键】：必须先用 setOutputFunction 触碰一次 logger 的函数内静态，
// 再注册钩子（atexit 是 LIFO）。详见构造函数里那两段注释。
inline std::function<void()> &kafkaShutdownHook()
{
    static std::function<void()> hook;
    return hook;
}

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

        // ── 退出路径防崩（第 1 步）：先「触碰」日志器的函数内静态 ──────────────
        //
        // 【症状】端口被占时进程以 SIGABRT(134) 退出，而不是干净的 exit(1)。
        //
        // 【成因链（已实测复现 + 崩溃报告比对确认）】
        //   1) drogon 在 createListeners() 里做端口探测
        //      （ListenerManager.cc:104 构造临时 TcpServer），bind 失败 ⇒
        //      trantor Socket::bindAddress() 直接 `LOG_SYSERR; exit(1);`
        //      （trantor/net/inner/Socket.cc:67-68）。
        //   2) `exit()` 只跑 atexit + 静态析构，【不等其他线程】。
        //      此刻 Kafka poll 线程仍在往 TBB 投递、TBB worker 仍在 LOG_ERROR。
        //   3) 日志器的两个【函数内静态】outputFunc_()/flushFunc_()
        //      （trantor/utils/Logger.h:291-302）随静态析构被销毁 —— 里面那个
        //      lambda 持有 asyncFileLoggerPtr_ 的 shared_ptr 副本
        //      （HttpAppFrameworkImpl.cc:1230-1235），lambda 一销毁，
        //      AsyncFileLogger 的 mutex 就没了。
        //   4) 还在跑的 TBB worker 接着 ~Logger() → AsyncFileLogger::flush()
        //      在已死的 mutex 上加锁 ⇒ std::system_error(EINVAL) ⇒
        //      从析构里抛出 ⇒ std::terminate ⇒ abort(134)。
        //      崩溃报告栈：__throw_system_error ← AsyncFileLogger::flush ←
        //      ~Logger ← AsyncKafkaConsumer::submitMessageTask 的 lambda ←
        //      TbbCoroutinePool::submit 的 lambda ← tbb worker。
        //
        // 【修法】利用 atexit 的 LIFO 语义：atexit 处理器与静态析构共用一个栈，
        //   按「注册/构造顺序的逆序」执行。所以先【触碰一次】那两个静态让它们
        //   在此刻完成构造，再注册退出钩子 —— 钩子必然排在它们析构之前。
        //
        //   放在构造函数最前面（早于两个 Kafka 消费者静态）也有讲究：
        //   消费者析构里还有 `LOG_DEBUG << "...consumer stopped."`，构造越早
        //   ⇒ 析构越晚 ⇒ 那条日志也落在日志器还活着的时候。
        //
        //   ⚠️ 不能依赖「某条 LOG_* 会先初始化它」：log_level=WARN 时
        //      LOG_DEBUG/LOG_INFO 宏直接短路（trantor Logger.h 的宏会先判级别），
        //      Logger 临时对象根本不构造，静态也就从没被初始化过。
        //   ⚠️ 不能放在 beginningAdvice 里：它在 HttpAppFrameworkImpl.cc:675
        //      才触发，而端口探测在 623 行（createListeners）—— 端口冲突这条路
        //      根本走不到 advice。实测：advice 版本 5/5 仍 abort(134)。
        //   ⚠️ 这里 setOutputFunction 传的就是 trantor 的默认行为
        //      （defaultOutputFunction/defaultFlushFunction 是 protected，
        //        外部取不到，只能等价重写）。若配置了 log_path，
        //      setupFileLogger()（HttpAppFrameworkImpl.cc:1230）紧接着就会把它
        //      覆盖成文件日志；若没配，行为与默认完全一致 —— 不改变语义。
        trantor::Logger::setOutputFunction(
            [](const char *msg, const uint64_t len)
            { std::fwrite(msg, 1, static_cast<size_t>(len), stdout); },
            [] { std::fflush(stdout); });

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

        // ── 退出路径防崩（第 2 步）：填装并注册钩子 ────────────────────────────
        //
        // 钩子体：停 Kafka 消费者（join poll 线程）→ 排空 TBB 在途任务。
        // 两步都幂等，析构函数稍后还会再调一次，无副作用。
        //
        // 注册时机必须【晚于】上面那次 setOutputFunction —— atexit 是 LIFO，
        // 越晚注册越早执行，这样才能抢在日志器静态析构之前动手。
        App::kafkaShutdownHook() = [] {
            asyncKafkaConsumer.requestStop();
            asyncKafkaConsumerOne.requestStop();
            TbbCoroutinePool::instance().waitAll();
        };

        std::atexit([] {
            try
            {
                if (auto &hook = App::kafkaShutdownHook())
                {
                    hook();
                }
            }
            catch (...)
            {
                // 退出路径上不允许任何异常逃逸：抛出去就是 terminate/abort
            }
        });

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

    // 注意：/metrics **不在这里**注册。
    //
    // 它由 config.json 的 `simple_controllers_map` 注册到 MetricsCtrl。
    // 这里曾经有一段代码级 `registerHandler("/metrics", ...)`，注释写着
    // 「避免反射加载顺序失效」—— 那是多余的：drogon 的
    // HttpControllersRouter::route() **先查 simpleCtrlMap_（配置注册）并直接
    // 返回**，查不到才轮到 ctrlMap_（registerHandler）。也就是说配置那条路
    // 一旦命中，这段代码**永远不会被执行**，是一段纯死代码，且它与
    // MetricsCtrl 各自维护一份响应构造逻辑（上一轮 setExpiredTime 的坑就
    // 两处都要改）。已删除，只保留 MetricsCtrl 一条路。
    //
    // 代价：/metrics 现在依赖 config.json 里那条 simple_controllers_map 存在
    // （与 /test 同机制）。改动配置时注意别把它删掉。

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
