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

namespace
{
std::string formUrl(const std::string& fqUrl)
{
    if (utils::startsWith("http://", fqUrl))
    {
        const auto urlStart = fqUrl.find('/', 7);
        const auto paramStart = fqUrl.find('?', urlStart + 2);
        if (paramStart != std::string::npos)
        {
            return fqUrl.substr(urlStart, paramStart - urlStart);
        }

        return fqUrl.substr(urlStart);
    }

    return fqUrl;
}
} // namespace

httpd::Response HttpdFactory::sendRequest(httpd::Method method, const char* url, const char* body)
{
    httpd::Request request(method, formUrl(url).c_str());
    request.body.append(body, strlen(body));
    std::string fqUrl = url;
    if (utils::startsWith("http://", fqUrl))
    {
        const auto urlStart = fqUrl.find('/', 7);
        const auto paramStart = fqUrl.find('?', urlStart + 2);
        if (paramStart != std::string::npos)
        {
            const auto params = fqUrl.substr(paramStart + 1);
            for (auto keyValueToken = utils::StringTokenizer::tokenize(params.c_str(), params.size(), "&");
                 !keyValueToken.empty();
                 keyValueToken = utils::StringTokenizer::tokenize(keyValueToken, "&"))
            {
                auto paramSplit = utils::StringTokenizer::tokenize(keyValueToken.start, keyValueToken.length, "=");
                if (!paramSplit.next)
                {
                    request.params.emplace(keyValueToken.str(), "");
                }
                else
                {
                    request.params.emplace(paramSplit.str(), utils::StringTokenizer::tokenize(paramSplit, '&').str());
                }
            }
        }
    }
    else
    {
        assert(false);
    }

    return _requestHandler->onRequest(request);
}

} // namespace emulator
