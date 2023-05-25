#pragma once
#include "httpd/HttpDaemon.h"
#include "httpd/Httpd.h"
#include "httpd/Request.h"
#include "httpd/Response.h"
#include "nlohmann/json.hpp"
#include "test/transport/FakeNetwork.h"

namespace fakenet
{
class Gateway;
}

namespace emulator
{

class HttpdFactory final : public httpd::HttpDaemonFactory
{
public:
    HttpdFactory() {}

    std::unique_ptr<httpd::HttpDaemon> create(httpd::HttpRequestHandler& requestHandler) override;

    httpd::Response sendRequest(httpd::Method method, const char* url, const char* body);

private:
    httpd::HttpRequestHandler* _requestHandler;
};

template <typename RequestT>
bool awaitResponse(HttpdFactory* httpd,
    const std::string& url,
    const std::string& body,
    const uint64_t timeout,
    nlohmann::json& outBody)
{
    if (httpd)
    {
        auto response = httpd->sendRequest(RequestT::method, url.c_str(), body.c_str());
        outBody = nlohmann::json::parse(response._body);
        return true;
    }
    else
    {
        RequestT request(url.c_str(), body.c_str());
        request.awaitResponse(timeout);

        if (request.isSuccess())
        {
            outBody = request.getJsonBody();
            return true;
        }
        else
        {
            logger::warn("request failed %d", "awaitResponse", request.getCode());
        }
        return false;
    }
}

template <typename RequestT>
bool awaitResponse(HttpdFactory* httpd, const std::string& url, const uint64_t timeout, nlohmann::json& outBody)
{
    if (httpd)
    {
        auto response = httpd->sendRequest(RequestT::method, url.c_str(), "");
        if (!response._body.empty())
        {
            outBody = nlohmann::json::parse(response._body);
        }
        return true;
    }
    else
    {
        RequestT request(url.c_str());
        request.awaitResponse(timeout);

        if (request.isSuccess())
        {
            outBody = request.getJsonBody();
            return true;
        }
        else
        {
            logger::warn("request failed %d", "awaitResponse", request.getCode());
        }
        return false;
    }
}

} // namespace emulator
