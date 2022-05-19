#pragma once

#include "httpd/Response.h"
#include <cstring>
#include <exception>

namespace httpd
{

class RequestErrorException : public std::exception
{
    constexpr static const char* ERROR_WHAT_START = "Status: ";
    constexpr static const char* ERROR_WHAT_MESSAGE = "\n message: ";
    constexpr static uint32_t STATUS_CODE_MAX_LEN = 10;
public:
    RequestErrorException(StatusCode statusCode, const char* message)
        : RequestErrorException(statusCode, std::string(message))
    { }

    RequestErrorException(StatusCode statusCode, const std::string& message)
        : RequestErrorException(statusCode, std::string(message))
    { }

    RequestErrorException(StatusCode statusCode, std::string&& message)
        : _statusCode(statusCode)
        , _message(std::move(message))
    { }

    const char* what() const noexcept override { return _message.c_str(); }
    const std::string& getMessage() const { return _message; }
    StatusCode getStatusCode() const noexcept { return _statusCode; }

private:
    StatusCode _statusCode;
    std::string _message;
};

} // namespace httpd
