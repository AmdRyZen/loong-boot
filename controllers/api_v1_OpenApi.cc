#include "api_v1_OpenApi.h"
#include "nlohmann/json.hpp"
#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"
#include "threadPool/threadPool.h"
#include "utils/redisUtils.h"
#include <drogon/HttpClient.h>
#include <taskflow/taskflow.hpp>  // Taskflow is header-only
#include "boost/version.hpp"
#include "boost/regex.hpp"
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/format.hpp>
#include "utils/opensslCrypto.h"
#include <boost/multiprecision/cpp_dec_float.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <execution> // 可能需要此头文件
#include <glaze/glaze.hpp>
#include "base/base.h"
#include "base/vo/data_vo.h"
#include "mqttManager/MqttManager.h"

#if defined(__arm__) || defined(__aarch64__)
    #include <arm_neon.h>
#else
    // 在非 ARM 架构上包含其他头文件，例如 <emmintrin.h>（SSE2 指令集的头文件）
    #include <emmintrin.h>
#endif


using namespace api::v1;
using namespace drogon;
using json = nlohmann::json;
using namespace boost::gregorian;
namespace mp = boost::multiprecision;
using namespace rapidjson;


Task<> OpenApi::mqtt(const HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback)
{
    // 发布消息
    MqttManager::instance().publish("topic", "Hello, MQTT!");

    co_return callback(Base<std::string>::createHttpSuccessResponse(StatusOK, Success, ""));
}

struct Task23
{
    struct promise_type
    {
        constexpr Task23 get_return_object()
        {
            return Task23{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        static std::suspend_never initial_suspend()
        {
            return {};
        }
        static std::suspend_always final_suspend() noexcept
        {
            return {};
        }
        static void return_void() {}
        static void unhandled_exception()
        {
            std::terminate();
        }
    };

    std::coroutine_handle<promise_type> coro;

     explicit Task23(std::coroutine_handle<promise_type> const h) : coro(h) {}
    ~Task23() { if (coro) coro.destroy(); }

    void resume() const {
        if (coro) coro.resume();
    }
};

Task23 example(const int n) {
    for (int i = 0; i < n; ++i) {
        co_await std::suspend_always{};
    }
}

void run_test(const int iterations) {
    const auto start = std::chrono::high_resolution_clock::now();
    const auto t = example(iterations);
    for (int i = 0; i < iterations; ++i) {
        t.resume();
    }
    const auto end = std::chrono::high_resolution_clock::now();
    const std::chrono::duration<double, std::nano> diff = end - start;
    const double average_time_ns = diff.count() / iterations;
    std::cout << std::format("Total Duration: {}", diff.count()) << " ns" << std::endl;
    std::cout << std::format("Average Time per Switch: {}", average_time_ns) << " ns" << std::endl;
}

Task<> OpenApi::coroutine(const HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback)
{
    constexpr int num_threads = 8;
    std::vector<std::thread> threads;

    for (int i = 0; i < num_threads; ++i) {
        constexpr int iterations = 10000000;
        threads.emplace_back(run_test, iterations);
    }

    for (auto& t : threads) {
        t.join();
    }

    co_return callback(Base<std::string>::createHttpSuccessResponse(StatusOK, Success, ""));
}

Task<> OpenApi::algorithm(const HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback)
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(-10000000, 10000000); // 生成-1000到1000之间的随机数，包括负数

    std::vector<int> vec(1000000);
    std::ranges::generate(vec.begin(), vec.end(), [&]() { return dis(gen); });

    auto vec_copy = vec;
    auto vec_copy1 = vec;
    auto vec_copy2 = vec;

    auto start = std::chrono::high_resolution_clock::now();
    std::ranges::sort(vec);
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    std::cout << "std::ranges::sort: " << elapsed.count() << " seconds\n";

    start = std::chrono::high_resolution_clock::now();
    std::stable_sort(std::execution::par, vec_copy.begin(), vec_copy.end());
    end = std::chrono::high_resolution_clock::now();
    elapsed = end - start;
    std::cout << "std::ranges::stable_sort: " << elapsed.count() << " seconds\n";

    start = std::chrono::high_resolution_clock::now();
    // C++17 引入了并行算法，通过指定执行策略（如 std::execution::par），可以利用多线程和多核处理器来加速排序。
    std::sort(std::execution::par, vec_copy1.begin(), vec_copy1.end());
    end = std::chrono::high_resolution_clock::now();
    elapsed = end - start;
    std::cout << "std::sort std::execution::par: " << elapsed.count() << " seconds\n";

    start = std::chrono::high_resolution_clock::now();
    // std::execution::par_unseq 是一种并行执行策略，其含义是允许算法在多个线程上并行执行，并且在某些情况下可以使用向量化来优化性能。
    // 这种策略的主要意义在于可以更充分地利用现代多核处理器和 SIMD（Single Instruction, Multiple Data）指令集，从而加速算法的执行。
    std::sort(std::execution::par_unseq, vec_copy2.begin(), vec_copy2.end());
    end = std::chrono::high_resolution_clock::now();
    elapsed = end - start;
    std::cout << "std::sort std::execution::par_unseq: " << elapsed.count() << " seconds\n";

    std::sort(vec.begin() + 2, vec.begin() + 7); // 只排序索引从2到6的元素

    const bool found = std::ranges::binary_search(vec.begin(), vec.end(), 1003);
    std::cout << "found: " << found << "\n";

    const auto sum = std::accumulate(vec.begin(), vec.end(), 0);
    std::cout << "sum: " << sum << "\n";

    std::ranges::for_each(vec, [](int &x) { x += 1; });
    auto sum1 = 0;
    for (const int v : vec)
    {
        sum1 += v;
    }
    std::cout << "sum1: " << sum1 << "\n";

    for(const auto& elem : vec) {
        if (elem < 0) {
            //std::cout << "Element: " << elem << "\n";
        }
    }

    const auto max_value = *std::ranges::max_element(vec);
    const auto min_value = *std::ranges::min_element(vec);
    std::cout << "Max element: " << max_value << ", Min element: " << min_value << "\n";

    // 已排序的向量
    const bool is_sorted_flag = std::ranges::is_sorted(vec);
    std::cout << "Is sorted: " << (is_sorted_flag ? "Yes" : "No") << "\n";

    std::ranges::reverse(vec);

    // 已按倒序排列的向量
    const bool is_desc_sorted_flag = std::ranges::is_sorted(vec, std::greater<>());
    std::cout << "Is desc_sorted_flag: " << (is_desc_sorted_flag ? "Yes" : "No") << "\n";

    // 排序是为了确保相同元素相邻，这样才能正确去重
    // 使用 std::unique 移动相邻的重复元素到末尾，并返回新的结尾
    // 使用 std::ranges::unique 移动相邻的重复元素到末尾，并返回新的范围
    auto unique_end = std::ranges::unique(vec);

    // 计算去重后的和
    const auto new_sum = std::accumulate(vec.begin(), unique_end.begin(), 0);
    std::cout << "Sum of unique elements: " << new_sum << '\n';

    // 去掉末尾重复元素
    vec.erase(unique_end.begin(), vec.end());

    // 输出最终去重后的向量的和  new_sum == new_sum1
    const auto new_sum1 = std::accumulate(vec.begin(), vec.end(), 0);
    std::cout << "Sum1 of unique elements: " << new_sum1 << '\n';


    //std::set_union: 计算两个有序范围的并集。
    //std::set_intersection: 计算两个有序范围的交集。
    //std::set_difference: 计算两个有序范围的差集。
    //std::set_symmetric_difference: 计算两个有序范围的对称差集。
    std::vector<int> result;
    std::ranges::set_union(vec.begin(), vec.end(), vec_copy.begin(), vec_copy.end(), std::back_inserter(result));
    std::cout << "set_union size: " << result.size() << '\n';

    std::vector<int> result1;
    std::ranges::set_intersection(vec.begin(), vec.end(), vec_copy.begin(), vec_copy.end(), std::back_inserter(result1));
    std::cout << "set_intersection size: " << result1.size() << '\n';

    std::vector<int> result2;
    std::ranges::set_difference(vec.begin(), vec.end(), vec_copy.begin(), vec_copy.end(), std::back_inserter(result2));
    std::cout << "set_difference size: " << result2.size() << '\n';

    std::vector<int> result3;
    std::ranges::set_symmetric_difference(vec.begin(), vec.end(), vec_copy.begin(), vec_copy.end(), std::back_inserter(result3));
    std::cout << "set_symmetric_difference size: " << result3.size() << '\n';

    co_return callback(Base<std::string>::createHttpSuccessResponse(StatusOK, Success, ""));
}


Task<> OpenApi::aes(const HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback)
{
    AesResponseDataVo aes_response_data_vo{};

    try
    {
        const std::string input = "123456";

        aes_response_data_vo.hash = opensslCrypto::sha3_256(input);
        //std::cout << "SHA-256 Hash: " << hash << std::endl;

        // 不建议使用md5 虽然性能更好
        aes_response_data_vo.md5_hash = opensslCrypto::md5(input);
        //std::cout << "MD5: " << md5_hash << std::endl;

        const std::string keyHex = "0123456789abcdef0123456789abcdef";
        const std::string ivHex = "0123456789abcdef0123456789abcdef";
        const std::string plaintext = "谢谢谢谢谢寻寻👀👀👀👀你好啊👀👀xxxx";

        aes_response_data_vo.encrypted = opensslCrypto::AesCBCPk5EncryptBase64(plaintext, keyHex, ivHex);
        //std::cout << "Encrypted: " << encrypted << std::endl;

        aes_response_data_vo.decrypted = opensslCrypto::AesCBCPk5DecryptBase64(aes_response_data_vo.encrypted, keyHex, ivHex);
        //std::cout << "Decrypted: " << decrypted << std::endl;

    } catch (const std::exception& e)
    {
        std::cout << "aes: err  " << e.what() << std::endl;
    }
    co_return callback(Base<AesResponseDataVo>::createHttpSuccessResponse(StatusOK, Success, aes_response_data_vo));
}


Task<> OpenApi::simd(const HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback)
{

    // 定义两个 NEON 整型向量
    constexpr int32x4_t a = {1, 2, 3, 4};
    constexpr int32x4_t b = {5, 6, 7, 8};

    // 执行向量加法操作
    int32x4_t result = vaddq_s32(a, b);

    // 将结果转换为标准整型数组
    int32_t res[4];
    vst1q_s32(res, result);

    // 创建一个包含 5 个整数的静态数组
    std::vector<int> myVector;

    for (const int& re : res)
    {
        myVector.push_back(re);
    }

    co_return callback(Base<std::vector<int>>::createHttpSuccessResponse(StatusOK, Success, myVector));
}

// 不会丢失精度
void add_arrays(const mp::cpp_dec_float_100* a, const mp::cpp_dec_float_100* b, mp::cpp_dec_float_100* result, const int size) {
    for (int i = 0; i < size; ++i) {
        result[i] = a[i] + b[i];
    }
}

#define ARRAY_SIZE 4
Task<> OpenApi::boost(const HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback)
{
    std::cout << BOOST_LIB_VERSION << std::endl;
    std::cout << BOOST_VERSION << std::endl;


    mp::cpp_dec_float_100 a1[ARRAY_SIZE] = {1.1, 2.2, 3.3, 4.4};
    mp::cpp_dec_float_100 b1[ARRAY_SIZE] = {5.5, 6.6, 7.7, 8.8};
    mp::cpp_dec_float_100 result[ARRAY_SIZE];

    add_arrays(a1, b1, result, ARRAY_SIZE);

    std::cout << "Result:" << std::endl;
    for (const auto & i : result) {
        std::cout <<  std::fixed << std::setprecision(2) << i << "； ";
    }
    std::cout << std::endl;


    // 定义固定精度的十进制浮点数，精度为100位
    mp::cpp_dec_float_100 money("123.456789");

    // 进行计算
    money += 10; // 加上10元

    // 设置输出格式并输出结果
    std::cout << "Money: " << std::fixed << std::setprecision(2) << money << std::endl;


    // 定义两个高精度的十进制浮点数
    mp::cpp_dec_float_100 a("123.456789");
    mp::cpp_dec_float_100 b("987.654321");

    // 加法
    mp::cpp_dec_float_100 sum = a + b;
    std::cout << "Sum: " << sum << std::endl;

    // 减法
    mp::cpp_dec_float_100 diff = a - b;
    std::cout << "Difference: " << diff << std::endl;

    // 乘法
    mp::cpp_dec_float_100 prod = a * b;
    std::cout << "Product: " << prod << std::endl;

    // 除法
    mp::cpp_dec_float_100 quot = a / b;
    std::cout << "Quotient: " << quot << std::endl;


    std::cout << "multiprecision end : ------------------" << std::endl;

    // std::async函数启动一个异步任务，传入func1和一个int值作为参数
    //std::future<void> f = std::async(printerFunc);

    // 在主线程中做一些其他事情
    std::cout << "Doing something else in main thread." << std::endl;

    std::cout << boost::format("Hello %s! You are %d years old.") % "Tom" % 25 << std::endl;

    std::string email = "someone@example.com";
    boost::regex pattern(R"(\b[a-zA-Z0-9._%-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,4}\b)");
    if (boost::regex_match(email, pattern))
        std::cout << "Valid email address." << std::endl;
    else
        std::cout << "Invalid email address." << std::endl;

    boost::random::mt19937 rng;                            // create a random number generator
    boost::random::uniform_int_distribution<> dist(0, 9);  // create a uniform distribution
    for (int i = 0; i < 10; ++i)                           // generate and print 10 random numbers
        std::cout << dist(rng) << " ";
    std::cout << std::endl;

    // 创建一个date对象，表示今天的日期
    date today = day_clock::local_day();
    std::cout << "Today is: " << today << std::endl;

    // 创建一个date对象，表示2023年2月14日
    date valentine(2023, Feb, 14);
    std::cout << "Valentine's day is: " << valentine << std::endl;

    // 创建一个date对象，表示从字符串解析的日期
    date birthday(from_string("2000-01-01"));
    std::cout << "Birthday is: " << birthday << std::endl;

    // 计算两个日期之间的差值，返回一个date_duration对象
    date_duration days_to_valentine = valentine - today;
    std::cout << "Days to Valentine's day: " << days_to_valentine.days() << std::endl;

    co_return callback(Base<std::string>::createHttpSuccessResponse(StatusOK, Success, ""));
}

// Add definition of your processing function here
void OpenApi::curlPost(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback)
{
    std::string key_string = "my_secret_key";
    /*AESCipher aes(key_string);

    std::string plaintext = "Hello, World!";
    std::string ciphertext = aes.encrypt(plaintext);
    std::string decryptedtext = aes.decrypt(ciphertext);

    std::cout << "Plaintext: " << plaintext << std::endl;
    std::cout << "Ciphertext: " << ciphertext << std::endl;
    std::cout << "Decryptedtext: " << decryptedtext << std::endl;*/



    const std::string url = "http://127.0.0.1:9090";
    std::string param = "id=222230&bb=xxxxx&cc=6&dd=10000&ip=127.0.0.1&zz=0&ff=11111111";
    std::string aes128Key = "xxxxxxxx";

    const auto client = drogon::HttpClient::newHttpClient(url);
    // aes
    /*std::string encryptedText = cipherUtils::encrypt_cbc(SbcConvertService::ws2s(SbcConvertService::s2ws(param)), aes128Key, aes128Key);
    std::cout << "encryptedText = " << encryptedText << std::endl;
    const std::string& pwd = cipherUtils::decrypt_cbc(encryptedText, aes128Key, aes128Key);
    std::cout << "pwd = " << pwd << std::endl;*/

    // md5
    /*unsigned char encrypt[] = "id=1111&timestamp=1638263374&key=xxxxx";
    unsigned char decrypt[16];*/
    /*MD5_CTX md5;
    md5Utils::MD5Init(&md5);
    md5Utils::MD5Update(&md5, encrypt, (int)strlen((char*)encrypt));
    md5Utils::MD5Final(&md5, decrypt);
    for (unsigned char i : decrypt)
    {
        printf("%02x", i);
    }
    std::cout << std::endl;*/

    Json::Value params;
    params["channelId"] = "1111111111";
    params["timestamp"] = "1638263374";
    params["param"] = "encryptedText";
    params["sign"] = "8482024d6c64fe364873725ea0e19008";

    std::cout << "__params = " << params.toStyledString() << std::endl;

    const auto request = drogon::HttpRequest::newHttpRequest();
    request->addHeader("Content-Type", "application/json");
    request->setBody(params.toStyledString());
    request->setMethod(drogon::Post);
    request->setPath("/api/v1/curlPost");
    request->setPathEncode(true);
    client->sendRequest(
        request,
        [](ReqResult result, const HttpResponsePtr& response) {
            if (result != ReqResult::Ok)
            {
                std::cout
                    << "error while sending request to server! result: "
                    << result << std::endl;
                return;
            }

            if (200 != response->statusCode())
            {
                std::cout << "cpp-demo : 接收反馈error = " << response->statusCode() << std::endl;
            }
            std::cout << "cpp-demo : 接收反馈result = " << response->getBody() << std::endl;
        });

    callback(Base<std::string>::createHttpSuccessResponse(StatusOK, Success, ""));
}

Task<> OpenApi::getValue(const HttpRequestPtr req,
                         std::function<void(const HttpResponsePtr&)> callback)
{
    const std::string command = std::format("get {}", "aa");
    const std::string redis_value = co_await redisUtils::getCoroRedisValue(command);

    co_return callback(Base<std::string>::createHttpSuccessResponse(StatusOK, Success, redis_value));
}

Task<> OpenApi::fastJson(const HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback)
{
    std::string buffer{};
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    std::cout << "Protocol Buffers version: " << GOOGLE_PROTOBUF_VERSION << std::endl;

    /*auto t1 = std::chrono::steady_clock::now();
    for (auto i = 0; i < 1000; i++)
    {
        // protobuf
        dto::UserData userData;
        userData.set_id("999");
        userData.set_name("protobuf");
        userData.set_message("message");
        userData.SerializeToString(&buffer);
    }
    auto t2 = std::chrono::steady_clock::now();
    //纳秒级
    double dr_ns = std::chrono::duration<double, std::nano>(t2 - t1).count();
    //微妙级
    double dr_us = std::chrono::duration<double, std::micro>(t2 - t1).count();
    std::cout << "[pb cost: " << dr_ns << " ns]" << std::endl;
    std::cout << "[pb cost: " << dr_us << " us]" << std::endl;*/

    //------------------解析----------------------
    /*dto::UserData rsp2{};
    if (!rsp2.ParseFromString(buffer))
    {
        std::cout << "parse error\n";
    }
    auto name = rsp2.name();
    // 清理 Protobuf 库
    google::protobuf::ShutdownProtobufLibrary();
    std::cout << "name:" << name << std::endl;*/

    // jsoncpp
    const auto t3 = std::chrono::steady_clock::now();
    for (auto i = 0; i < 1000; i++)
    {
        Json::Value root;
        root["id"] = 1;
        root["name"] = "b";
        buffer = root.toStyledString();
    }
    const auto t4 = std::chrono::steady_clock::now();
    //纳秒级
    const double dr_ns1 = std::chrono::duration<double, std::nano>(t4 - t3).count();
    std::cout << "[jsoncpp cost: " << std::format("{}", dr_ns1) << " ns]" << std::endl;

    // rapidjson
    const auto t5 = std::chrono::steady_clock::now();
    for (auto i = 0; i < 1000; i++)
    {
        StringBuffer buf;
        PrettyWriter writer(buf);  // it can word wrap
        writer.StartObject();
        writer.Key("id");
        writer.Int(1);
        writer.Key("name");
        writer.String("a");
        writer.EndObject();
        buffer = buf.GetString();
    }
    const auto t6 = std::chrono::steady_clock::now();
    //纳秒级
    const double dr_ns2 = std::chrono::duration<double, std::nano>(t6 - t5).count();
    std::cout << "[rapidjson cost: " << std::format("{}", dr_ns2) << " ns]" << std::endl;

    // onlohmannJson
    const auto t7 = std::chrono::steady_clock::now();
    for (auto i = 0; i < 1000; i++)
    {
        json onlohmannJson;
        onlohmannJson["id"] = 1;
        onlohmannJson["name"] = "c";
        buffer = onlohmannJson.dump();
    }
    const auto t8 = std::chrono::steady_clock::now();
    //纳秒级
    const double dr_ns3 = std::chrono::duration<double, std::nano>(t8 - t7).count();
    std::cout << "[onlohmannJson cost: " << std::format("{}", dr_ns3) << " ns]" << std::endl;

    // glaze
    const auto t9 = std::chrono::steady_clock::now();
    for (auto i = 0; i < 1000; i++)
    {
        MyStruct s{};
        // BEVE
        (void) glz::write_json(s, buffer);
        // glz::write_binary(s, buffer);
    }
    const auto t10 = std::chrono::steady_clock::now();
    //纳秒级
    const double dr_ns4 = std::chrono::duration<double, std::nano>(t10 - t9).count();
    std::cout << "[glaze cost: " <<  std::format("{}", dr_ns4) << " ns]" << std::endl;

    std::cout << "---------------xx-----------------" << std::endl;

    MyStruct my_struct{};
    (void) glz::read_json(my_struct, buffer);
    //co_return callback(Base<MyStruct>::createHttpSuccessResponse(StatusOK, Success, my_struct));

    // protobuf
    /*dto::UserData protobufResponse;
    if (!protobufResponse.ParseFromString(std::string(req->getBody())))
    {
        LOG_ERROR << "---------------ParseFromString error-----------------";
    }*/

    /*protobufResponse.set_id("200");
    protobufResponse.set_name("Hello, this is a Protobuf response");
    protobufResponse.set_message("我草");*/

    // 序列化到字符串
    /*std::string output;
    if (!protobufResponse.SerializeToString(&output)) {
        LOG_ERROR << "Failed to serialize message.";
    }
    // 将序列化后的数据写入文件
    std::ofstream ofs("message.bin", std::ios::binary);
    ofs.write(output.data(), static_cast<std::streamsize>(output.size()));
    ofs.close();*/

    LOG_INFO << "Binary file generated successfully.";

    co_return callback(Base<std::string>::createHttpSuccessResponse(StatusOK, Success, ""));
}

std::atomic<int64_t> value(0);
inline void threadF()
{
    for (int i = 0; i < 100; ++i)
    {
        ++value;
    }
}

int64_t value1 = 0;
std::mutex mtx;
inline void threadF1()
{
    for (int i = 0; i < 100; ++i)
    {
        std::lock_guard<std::mutex> lk(mtx);
        thread_local int count = 0;
        ++count;
        std::cout << "count: " << count << std::endl;
        // 当前线程休眠1毫秒
        //std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++value1;
    }
}

Task<> OpenApi::threadPool(const HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback)
{
    const auto result = pool.enqueue([] {
        threadF();
        threadF1();
    });
    const auto result1 = pool.enqueue([] {
        threadF();
        threadF1();
    });
    result.wait();
    result1.wait();
    std::cout << "value = " << value << std::endl;
    std::cout << "value1 = " << value1 << std::endl;

    // taskflow.github.io
    tf::Executor executor;
    tf::Taskflow taskflow;

    const auto clientPtr = drogon::app().getFastDbClient();
    const auto ret = co_await clientPtr->execSqlCoro("select count(1) from xxl_job_info where author != ?", "薯条三兄弟");
    auto count = ret[0][0].as<std::int32_t>();

    // now - use std::future instead
    std::future<bool> fu = executor.async([&count]() {
        std::cout << "async task returns boolean" << std::endl;
        std::cout << "count = " << count << std::endl;
        if (count >= 0)
        {
            return true;
        }
        return false;
    });
    fu.get();

    executor.silent_async([]() {
        std::cout << "async task of no return" << std::endl;
    });

    // launch an asynchronous task from a running task
    taskflow.emplace([&]() {
        executor.async([]() {
            std::cout << "async task within a task" << std::endl;
        });
    });

    executor.run(taskflow).wait();

    bool future_ret = false;
    if (fu.get())
    {
        future_ret = fu.get();
    }

    constexpr double foo = 0.0;
    constexpr double bar = 1.0;
    auto constexpr res = foo <=> bar;
    if (res < 0) [[likely]]
        std::cout << "foo 小于 bar" << std::endl;
    else if (res > 0) [[unlikely]]
    {
        [[unreachable]];
        std::cout << "foo 大于 bar" << std::endl;
    }
    else  // (res == 0)
        std::cout << "foo 与 bar 相等" << std::endl;

    co_return callback(Base<bool>::createHttpSuccessResponse(StatusOK, Success, future_ret));
}

Task<> OpenApi::fix(const HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback)
{
    // mysqlbinlog --no-defaults --base64-output=decode-rows -v ./mysql-bin.000131 --result-file=./2.sql

    const auto clientPtr = drogon::app().getFastDbClient();

    struct Data : public Json::Value
    {
        Json::Value data;
        Json::Value item;
        Data()
          : data()
        {}
    } _data;

    static_assert(std::alignment_of_v<Data> >= 8, "Data struct must be aligned to 8 bytes");

    /*template<
        std::size_t Len,
        std::size_t Align = */
    /*default-alignment*/ /*
        >
    struct aligned_storage;
    (since C++11)(deprecated in C++23)*/

    //static std::aligned_storage<sizeof(Data), alignof(Data)>::type data;
    //Data* attr = new (&data) Data;
    //attr->data["data"] = "aligned_storage";
    //std::cout << attr->data << std::endl;
    //std::cout << "attr = " << sizeof(attr) << std::endl;
    std::cout << "__data = " << sizeof(_data) << std::endl;

    auto v = co_await clientPtr->execSqlCoro("select user_id from xxxxx where  original_number != 0 and op_number != 0  group by user_id having count(1) > 1 order by create_time");
    for (auto && n : v)
    {
        auto userId = n["user_id"]. as<std::int32_t>();

        // ....
        auto result = co_await clientPtr->execSqlCoro("select * from xxxxxxxx where original_number != 0 and op_number != 0 and user_id = ? order by create_time desc", userId);

        for (std::size_t i = 0; i < result.size(); ++i)
        {
            auto original_number = result[i]["original_number"]. as<std::int32_t>();
            auto op_number = result[i]["op_number"]. as<std::int32_t>();

            int32_t next_original_number = 0;
            if (i < result.size() - 1)
            {
                next_original_number = result[i + 1]["original_number"]. as<std::int32_t>();
            }

            if ((original_number + op_number) != next_original_number && next_original_number != 0)
            {
                _data.item["record_id"] = result[i]["record_id"]. as<std::int32_t>();
                _data.item["user_id"] = result[i]["user_id"]. as<std::int32_t>();
                _data.item["original_number"] = result[i]["original_number"]. as<std::int32_t>();
                _data.item["op_number"] = result[i]["op_number"]. as<std::int32_t>();
                _data.item["next_original_number"] = next_original_number;
                _data.item["diff"] = original_number + op_number - next_original_number;

                _data.data.append(_data.item);
                _data.item.clear();
            }
        }
    }

    std::cout << "size  = " << _data.data.size() << std::endl;

    co_return callback(Base<Json::Value>::createHttpSuccessResponse(StatusOK, Success, _data.data));
}

int getRandomValue() {
    // 随机数引擎
    std::random_device rd;  // 获取一个随机数种子
    std::mt19937 gen(rd()); // 使用 Mersenne Twister 算法作为随机数引擎

    // 生成 0 到 1 的随机整数
    std::uniform_int_distribution<> dis(0, 1);

    return dis(gen);
}

Task<> OpenApi::random(const HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback)
{
    std::default_random_engine random(time(nullptr));

    std::uniform_int_distribution int_dis(0, 100);        // 整数均匀分布
    std::uniform_real_distribution<float> real_dis(0.0, 1.0);  // 浮点数均匀分布

    std::vector<int32_t> value;
    for (int i = 0; i < 10; ++i)
    {
        auto inta = int_dis(random);
        value.push_back(inta);
        std::cout << inta << ' ';
    }
    std::cout << std::endl;

    for (int i = 0; i < 10; ++i)
    {
        std::cout << real_dis(random) << ' ';
    }
    std::cout << std::endl;

    auto const [first, second] = std::minmax_element(value.begin(), value.end());
    std::cout << "min element at: " << *first << std::endl;
    std::cout << "max element at: " << *second << std::endl;
    std::ranges::sort(value.begin(), value.end());
    std::ranges::for_each(value.begin(), value.end(), [&](const auto& item) {
        std::cout << item << std::endl;
    });
    value.clear();

    co_return callback(Base<std::string>::createHttpSuccessResponse(StatusOK, Success, ""));
}

Task<> OpenApi::taskflow(HttpRequestPtr req, std::function<void(const HttpResponsePtr&)> callback)
{
    tf::Executor executor;
    tf::Taskflow taskflow;

    // 创建两个计数器
    int count1 = 0;
    int count2 = 0;

    // 创建一个循环，执行 5 次
    auto loop = taskflow.emplace([&count1](){
                            std::cout << "Loop iteration " << count1 << std::endl;
                            count1++;
                        }).name("loop");

    // 创建一个条件任务，当 count2 小于 3 时执行
    auto condition = taskflow.emplace([&count2](){
                                 std::cout << "Condition check " << count2 << std::endl;
                                 count2++;
                                 return count2 < 3;
                             }).name("condition");

    // 设置任务之间的依赖关系
    condition.precede(loop);
    //condition.succeed(loop, std::ranges::any_of);

    // 执行任务流
    executor.run(taskflow).wait();

    std::cout << "-----------------" << std::endl;

    auto [A, B, C, D] = taskflow.emplace(  // create four tasks
        [] () { std::cout << "TaskA\n"; },
        [] () { std::cout << "TaskB\n"; },
        [] () { std::cout << "TaskC\n"; },
        [] () { std::cout << "TaskD\n"; }
    );

    A.precede(B, C);  // A runs before B and C
    D.succeed(B, C);  // D runs after  B and C

    executor.run(taskflow).wait();

    tf::Task A1 = taskflow.emplace([](){}).name("A");
    tf::Task C1 = taskflow.emplace([](){}).name("C");
    tf::Task D1 = taskflow.emplace([](){}).name("D");

    tf::Task B1 = taskflow.emplace([] (tf::Subflow& subflow) { // subflow task B
                             tf::Task B1 = subflow.emplace([](){}).name("B1");
                             tf::Task B2 = subflow.emplace([](){}).name("B2");
                             tf::Task B3 = subflow.emplace([](){}).name("B3");
                             B3.succeed(B1, B2);  // B3 runs after B1 and B2
                         }).name("B");

    A1.precede(B1, C1);  // A runs before B and C
    D1.succeed(B1, C1);  // D runs after  B and C

    tf::Task init = taskflow.emplace([](){}).name("init");
    tf::Task stop = taskflow.emplace([](){}).name("stop");

    // creates a condition task that returns a random binary
    tf::Task cond = taskflow.emplace([&](){ return getRandomValue(); }).name("cond");

    // creates a feedback loop {0: cond, 1: stop}
    init.precede(cond);
    cond.precede(cond, stop);  // moves on to 'cond' on returning 0, or 'stop' on 1

    // create asynchronous tasks directly from an executor
    std::future<int> future = executor.async([](){
        std::cout << "async task returns 1\n";
        return 1;
    });
    executor.silent_async([](){ std::cout << "async task does not return\n"; });

    // create asynchronous tasks with dynamic dependencies
    tf::AsyncTask A2 = executor.silent_dependent_async([](){ printf("A\n"); });
    tf::AsyncTask B2 = executor.silent_dependent_async([](){ printf("B\n"); }, A2);
    tf::AsyncTask C2 = executor.silent_dependent_async([](){ printf("C\n"); }, A2);
    tf::AsyncTask D2 = executor.silent_dependent_async([](){ printf("D\n"); }, B2, C2);

    executor.wait_for_all();

    co_return callback(Base<std::string>::createHttpSuccessResponse(StatusOK, Success, ""));
}

