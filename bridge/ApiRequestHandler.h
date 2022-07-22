#pragma once

#include "bridge/endpointActions/ActionContext.h"
#include "httpd/HttpRequestHandler.h"
#include <functional>
#include <map>
#include <string>

#if ENABLE_LEGACY_API
#include "bridge/LegacyApiRequestHandler.h"
#endif

namespace api
{
struct AllocateEndpoint;
struct EndpointDescription;
struct Recording;
} // namespace api

namespace transport
{
class SslDtls;
}

namespace utils
{
namespace StringTokenizer
{
struct Token;
}
} // namespace utils

namespace bridge
{

class Mixer;
class MixerManager;
struct StreamDescription;
class RequestLogger;

class ApiRequestHandler : public httpd::HttpRequestHandler, public ActionContext
{
public:
    ApiRequestHandler(bridge::MixerManager& mixerManager, transport::SslDtls& sslDtls);
    httpd::Response onRequest(const httpd::Request& request) override;

private:
    std::atomic<uint32_t> _lastAutoRequestId;
#if ENABLE_LEGACY_API
    std::unique_ptr<LegacyApiRequestHandler> _legacyApiRequestHandler;
#endif

    httpd::Response callEndpointAction(RequestLogger&, const httpd::Request&);

    using RequestAction = std::function<
        httpd::Response(ActionContext*, RequestLogger&, const httpd::Request&, const utils::StringTokenizer::Token&)>;
};

} // namespace bridge
