#pragma once

#include "httpd/HttpDaemon.h"
#include <cstdint>

namespace httpd
{

class Httpd : public HttpDaemon
{
public:
    explicit Httpd(HttpRequestHandler& httpRequestHandler);
    ~Httpd();

    bool start(const transport::SocketAddress& socketAddress) override;

private:
    struct OpaqueDaemon;
    OpaqueDaemon* _daemon;
    HttpRequestHandler& _httpRequestHandler;
};

} // namespace httpd
