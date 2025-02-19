#pragma once

#include "nlohmann/json.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace httpd
{

enum class StatusCode : uint32_t
{
    OK = 200,
    NO_CONTENT = 204,
    INTERNAL_SERVER_ERROR = 500,
    BAD_REQUEST = 400,
    NOT_FOUND = 404,
    METHOD_NOT_ALLOWED = 405
};

struct Response
{
    Response(const StatusCode statusCode, std::string&& body) : statusCode(statusCode), body(std::move(body)) {}
    Response(const StatusCode statusCode, const std::string& body) : statusCode(statusCode), body(body) {}
    explicit Response(const StatusCode statusCode) : statusCode(statusCode) {}

    Response(Response&& other)
        : statusCode(other.statusCode),
          body(std::move(other.body)),
          headers(std::move(other.headers))
    {
    }

    nlohmann::json getBodyAsJson() const { return nlohmann::json::parse(body); }

    const StatusCode statusCode;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
};

} // namespace httpd
