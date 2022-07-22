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

    enum class ApiActions
    {
        ABOUT,
        STATS,
        GET_CONFERENCES,
        ALLOCATE_CONFERENCE,
        GET_CONFERENCE_INFO,
        GET_ENDPOINT_INFO,
        PROCESS_CONFERENCE_ACTION,
        PROCESS_BARBELL_ACTION,
        LAST
    };

    ApiActions getAction(const httpd::Request&, utils::StringTokenizer::Token& outToken);

    using RequestAction = std::function<
        httpd::Response(ActionContext*, RequestLogger&, const httpd::Request&, const utils::StringTokenizer::Token&)>;
    std::map<ApiActions, RequestAction> _actionMap;
};

} // namespace bridge
