//
// Created by 神圣•凯莎 on 24-7-31.
//

#ifndef BASE_H
#define BASE_H

#include <glaze/glaze.hpp>
#include <google/protobuf/stubs/common.h>

using namespace drogon;

constexpr int StatusOK = 200;
constexpr int StatusError = 400;
constexpr std::string Success = "success";
constexpr std::string NoLogin = "NoLogin";
constexpr std::string Error = "error";

// 泛型 Base 结构体，封装 HTTP 响应数据
template <typename T>
struct Base {
    int code = StatusOK; // 默认响应码
    T data; // 泛型数据
    std::string message = Success; // 默认消息

    // 序列化为 JSON 的方法
    [[nodiscard]] std::string toJson() const {
        std::string json_output;
        (void) glz::write_json(*this, json_output);
        return json_output;
    }

    // 序列化为 Binary 的方法
    [[nodiscard]] std::vector<std::byte> toBinary() const {
        std::vector<std::byte> binary_output;
        (void) glz::write_beve(*this, binary_output);
        return binary_output;
    }

    // 静态方法，创建并返回 HTTP 响应  编译期多态：由于 Base 是模板类，所有操作（包括序列化和响应创建）都在编译期确定。这可以避免运行时开销，通常会有较好的性能。
    [[gnu::always_inline]] static HttpResponsePtr createHttpSuccessResponse(int statusCode = StatusOK, std::string_view message = Success, T data = T()) {
        Base<T> response;
        response.code = statusCode;
        response.message = message;
        response.data = std::move(data);

        // 创建 HTTP 响应对象
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k200OK);
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        resp->setBody(response.toJson());
        return resp;
    }

    // 静态方法，创建并返回 HTTP 响应  编译期多态：由于 Base 是模板类，所有操作（包括序列化和响应创建）都在编译期确定。这可以避免运行时开销，通常会有较好的性能。
    [[gnu::always_inline]] static HttpResponsePtr createHttpErrorResponse(int statusCode = StatusError, std::string_view message = Error, T data = T()) {
        Base<T> response;
        response.code = statusCode;
        response.message = message;
        response.data = std::move(data);

        // 创建 HTTP 响应对象
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k400BadRequest);
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        resp->setBody(response.toJson());
        return resp;
    }

    // 静态方法，创建并返回 HTTP 响应  编译期多态：由于 Base 是模板类，所有操作（包括序列化和响应创建）都在编译期确定。这可以避免运行时开销，通常会有较好的性能。
    [[gnu::always_inline]] static HttpResponsePtr createHttpUnauthorizedResponse(int statusCode = k401Unauthorized, std::string_view message = NoLogin, T data = T()) {
        Base<T> response;
        response.code = statusCode;
        response.message = message;
        response.data = std::move(data);

        // 创建 HTTP 响应对象
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k401Unauthorized);
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        resp->setBody(response.toJson());
        return resp;
    }

    // 静态方法，创建并返回 HTTP 响应  编译期多态：由于 Base 是模板类，所有操作（包括序列化和响应创建）都在编译期确定。这可以避免运行时开销，通常会有较好的性能。
    [[gnu::always_inline]] static HttpResponsePtr createHttpProtobufSuccessResponse(int statusCode = StatusOK, std::string_view message = Success, T data = T()) {
        // 创建 HTTP 响应并设置 Protobuf 二进制数据
        auto resp = HttpResponse::newHttpResponse();
        // 序列化 Protobuf 消息
        std::string serializedData;
        if (data.SerializeToString(&serializedData))
        {
            resp->setBody(std::move(serializedData));
        }
        // 设置内容类型为 Protobuf
        resp->setContentTypeCode(CT_APPLICATION_OCTET_STREAM);  // 或者使用 `application/protobuf`
        resp->setContentTypeString("application/x-protobuf");
        // 清理 Protobuf 库
        //google::protobuf::ShutdownProtobufLibrary();
        return resp;
    }
};

// Glaze 元信息定义：用于序列化 Base<T> 结构体
template <typename T>
struct glz::meta<Base<T>> {
    using TBase = Base<T>;
    static constexpr auto value = glz::object(
        "code", &TBase::code,
        "data", &TBase::data,
        "message", &TBase::message
    );
};

#endif //BASE_H
