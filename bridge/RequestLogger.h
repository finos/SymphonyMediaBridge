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

    ~RequestLogger()
    {
        logger::info("Outgoing response [%s] %u", "RequestHandler", _requestId.c_str(), _responseStatusCode);
    }

private:
    std::string _requestId;
    uint32_t _responseStatusCode;
};

} // namespace bridge
