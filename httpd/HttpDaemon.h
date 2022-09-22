#pragma once
#include <memory>

namespace transport
{
class SocketAddress;
}

namespace httpd
{
class HttpRequestHandler;

class HttpDaemon
{
public:
    virtual ~HttpDaemon() = default;
    virtual bool start(const transport::SocketAddress& socketAddress) = 0;
};

class HttpDaemonFactory
{
public:
    virtual ~HttpDaemonFactory() = default;
    virtual std::unique_ptr<HttpDaemon> create(httpd::HttpRequestHandler& requestHandler) = 0;
};
} // namespace httpd
