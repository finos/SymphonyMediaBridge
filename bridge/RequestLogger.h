#pragma once

#include "httpd/Request.h"
#include "logger/Logger.h"
#include <atomic>
#include <cstdint>
#include <string>

namespace bridge
{

class RequestLogger
{
public:
    RequestLogger(const httpd::Request& request, std::atomic<uint32_t>& lastAutoRequestId) : _responseStatusCode(0)
    {
        const auto traceIdHeader = request._headers.find("X-Trace-Id");
        _requestId = (traceIdHeader != request._headers.end() ? traceIdHeader->second
                                                              : std::to_string(1000 + (++lastAutoRequestId)));
        logger::info("Incoming request [%s] %s %s",
            "RequestHandler",
            _requestId.c_str(),
            request._methodString.c_str(),
            request._url.c_str());
    }

    void setResponse(const httpd::Response& response)
    {
        _responseStatusCode = static_cast<uint32_t>(response._statusCode);
    }

    void setErrorMessage(const std::string& message)
    {
        _errorMessages = message;
    }

    ~RequestLogger()
    {
        if (_errorMessages.empty())
        {
            logger::info("Outgoing response [%s] %u", "RequestHandler", _requestId.c_str(), _responseStatusCode);
        }
        else
        {
            logger::info("Outgoing response [%s] %u. Error message: %s", "RequestHandler", _requestId.c_str(), _responseStatusCode, _errorMessages.c_str());
        }
    }

private:
    std::string _requestId;
    std::string _errorMessages;
    uint32_t _responseStatusCode;
};

} // namespace bridge
