#pragma once
#include "HttpDaemon.h"

namespace httpd
{

class HttpdFactory : public HttpDaemonFactory
{
public:
    std::unique_ptr<HttpDaemon> create(httpd::HttpRequestHandler& requestHandler) override;
};
} // namespace httpd
