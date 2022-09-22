#pragma once
#include "httpd/HttpDaemon.h"
#include "httpd/Httpd.h"
#include "httpd/Request.h"
#include "httpd/Response.h"
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

} // namespace emulator
