#pragma once

#include "httpd/Request.h"
#include "httpd/Response.h"
#include "logger/Logger.h"
#include "utils/StdExtensions.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace bridge
{

class RequestLogger
{
public:
    RequestLogger(const httpd::Request& request, std::atomic<uint32_t>& lastAutoRequestId)
        : _request(request),
        _responseStatusCode(0)
    {
        if (shouldLog(request._url))
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
            if (!_requestId.empty())
            {
                logger::info("Outgoing response [%s] %u", "RequestHandler", _requestId.c_str(), _responseStatusCode);
            }
        }
        else if (!_requestId.empty())
        {
            logger::warn("Outgoing response [%s] %u. Error message: %s", "RequestHandler", _requestId.c_str(), _responseStatusCode, _errorMessages.c_str());
        }
        else
        {
            logger::error("Outgoing response for '%s' %u. Error message: %s", "RequestHandler", _request._url.c_str(), _responseStatusCode, _errorMessages.c_str());
        }
    }

private:
    bool shouldLog(const std::string& uri)
    {
        return _logFilter.end() == (std::find_if(_logFilter.begin(), _logFilter.end(),
                [&uri] (const auto& f) { return utils::startsWith(f, uri); }));
    }

private:
    const httpd::Request& _request;
    std::string _requestId;
    std::string _errorMessages;
    uint32_t _responseStatusCode;

    const static std::vector<std::string> _logFilter;
};

} // namespace bridge
