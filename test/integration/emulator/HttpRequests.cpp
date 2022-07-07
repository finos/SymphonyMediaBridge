#include "HttpRequests.h"
#include "logger/Logger.h"
#include "utils/Time.h"

namespace emulator
{

HttpPostRequest::HttpPostRequest(const char* url, const char* body)
    : _request(nullptr),
      _status(HTTP_STATUS_PENDING),
      _prevSize(0)
{
    _request = http_post(url, body, body ? std::strlen(body) : 0, nullptr);
}

HttpPostRequest::~HttpPostRequest()
{
    http_release(_request);
}

void HttpPostRequest::awaitResponse(uint64_t timeout)
{
    const auto startTime = utils::Time::getAbsoluteTime();

    while (_status == HTTP_STATUS_PENDING)
    {
        _status = http_process(_request);
        if (_prevSize != _request->response_size)
        {
            logger::debug("%zu byte(s) received.", "HttpPostRequest", _request->response_size);
            _prevSize = _request->response_size;
        }
        if (utils::Time::getAbsoluteTime() - startTime > timeout)
        {
            logger::error("Timeout waiting for response", "HttpPostRequest");
            _status = HTTP_STATUS_FAILED;
            break;
        }
        utils::Time::nanoSleep(2 * utils::Time::ms);
    }
}

std::string HttpPostRequest::getResponse() const
{
    if (isSuccess())
    {
        return (char const*)_request->response_data;
    }
    return "";
}

nlohmann::json HttpPostRequest::getJsonBody() const
{
    if (isSuccess())
    {
        return nlohmann::json::parse(static_cast<const char*>(_request->response_data));
    }
    return nlohmann::json();
}

} // namespace emulator
