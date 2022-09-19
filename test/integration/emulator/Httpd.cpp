#include "httpd/Httpd.h"
#include "httpd/HttpRequestHandler.h"
#include "httpd/Request.h"
#include "test/integration/emulator/Httpd.h"
#include "test/transport/FakeNetwork.h"
#include "utils/SocketAddress.h"
#include "utils/StdExtensions.h"
#include "utils/StringBuilder.h"
#include "utils/StringTokenizer.h"

namespace emulator
{
class FakeHttpd : public httpd::HttpDaemon
{
public:
    FakeHttpd() {}
    bool start(const transport::SocketAddress& socketAddress) override;

private:
};

bool FakeHttpd::start(const transport::SocketAddress&)
{
    return true;
}

std::unique_ptr<httpd::HttpDaemon> HttpdFactory::create(httpd::HttpRequestHandler& httpRequestHandler)
{
    _requestHandler = &httpRequestHandler;
    return std::make_unique<FakeHttpd>();
}

httpd::Response HttpdFactory::sendRequest(httpd::Method method, const char* url, const char* body)
{
    httpd::Request request(method);
    request._body.append(body);
    std::string fqUrl = url;
    if (utils::startsWith("http://", fqUrl))
    {
        auto urlStart = fqUrl.find('/', 7);
        request._url = fqUrl.substr(urlStart);
    }
    else
    {
        request._url = url;
    }

    return _requestHandler->onRequest(request);
}

} // namespace emulator
