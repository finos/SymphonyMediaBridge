#pragma once

#include <cstdint>

namespace transport
{
class SocketAddress;
} // namespace transport

namespace httpd
{

class HttpRequestHandler;

class Httpd
{
public:
    explicit Httpd(HttpRequestHandler& httpRequestHandler);
    ~Httpd();

    bool start(const transport::SocketAddress& socketAddress);

private:
    struct OpaqueDaemon;
    OpaqueDaemon* _daemon;
    HttpRequestHandler& _httpRequestHandler;
};

} // namespace httpd
