#pragma once

#include "httpd/Response.h"
#include "utils/StringBuilder.h"
#include <exception>

namespace httpd
{

class RequestErrorException : public std::exception
{
public:
    RequestErrorException(StatusCode statusCode) : _statusCode(statusCode)
    {
        utils::StringBuilder<64> result;
        result.append("RequestErrorException status: ");
        result.append(static_cast<uint32_t>(_statusCode));
        _what = result.build();
    }

    const char* what() const noexcept override { return _what.c_str(); }
    StatusCode getStatusCode() const noexcept { return _statusCode; }

private:
    StatusCode _statusCode;
    std::string _what;
};

} // namespace httpd
