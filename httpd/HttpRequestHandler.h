#pragma once

#include "httpd/Request.h"
#include "httpd/Response.h"

namespace httpd
{

class HttpRequestHandler
{
public:
    virtual ~HttpRequestHandler() = default;

    virtual Response onRequest(const Request& request) = 0;
};

} // namespace httpd
