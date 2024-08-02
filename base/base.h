//
// Created by 神圣•凯莎 on 24-7-31.
//

#ifndef BASE_H
#define BASE_H

#include <glaze/glaze.hpp>

using namespace drogon;

// 泛型 Base 结构体，封装 HTTP 响应数据
template <typename T>
struct Base {
    int code = 200; // 默认响应码
    T data; // 泛型数据
    std::string msg = "success"; // 默认消息

    // 序列化为 JSON 的方法
    [[nodiscard]] std::string toJson() const {
        std::string json_output;
        (void) glz::write_json(*this, json_output);
        return json_output;
    }

    // 静态方法，创建并返回 HTTP 响应  编译期多态：由于 Base 是模板类，所有操作（包括序列化和响应创建）都在编译期确定。这可以避免运行时开销，通常会有较好的性能。
    static HttpResponsePtr createHttpResponse(int statusCode = 200, const std::string& msg = "success", const T& data = T()) {
        Base<T> response;
        response.code = statusCode;
        response.msg = msg;
        response.data = std::move(data);

        // 创建 HTTP 响应对象
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k200OK);
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        resp->setBody(response.toJson());
        return resp;
    }
};

#endif //BASE_H
