#include "httpd/HttpdFactory.h"
#include "HttpDaemon.h"
#include "httpd/Httpd.h"

namespace httpd
{
class HttpRequestHandler;

std::unique_ptr<HttpDaemon> HttpdFactory::create(HttpRequestHandler& requestHandler)
{
    return std::make_unique<httpd::Httpd>(requestHandler);
}

} // namespace httpd
