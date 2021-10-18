#pragma once

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
    Response(const StatusCode statusCode, std::string&& body) : _statusCode(statusCode), _body(std::move(body)) {}
    Response(const StatusCode statusCode, const std::string& body) : _statusCode(statusCode), _body(body) {}
    explicit Response(const StatusCode statusCode) : _statusCode(statusCode) {}

    Response(Response&& other)
        : _statusCode(other._statusCode),
          _body(std::move(other._body)),
          _headers(std::move(other._headers))
    {
    }

    StatusCode _statusCode;
    std::string _body;
    std::unordered_map<std::string, std::string> _headers;
};

} // namespace httpd
