#pragma once

#include <cstdint>

namespace httpd
{

class HttpRequestHandler;

class Httpd
{
public:
    explicit Httpd(HttpRequestHandler& httpRequestHandler);
    ~Httpd();

    bool start(const uint32_t port);

private:
    struct OpaqueDaemon;
    OpaqueDaemon* _daemon;
    HttpRequestHandler& _httpRequestHandler;
};

} // namespace httpd
