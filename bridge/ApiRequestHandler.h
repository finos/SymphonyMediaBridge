#pragma once

#include "httpd/HttpRequestHandler.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#if ENABLE_LEGACY_API
#include "bridge/LegacyApiRequestHandler.h"
#endif

namespace api
{
struct AllocateEndpoint;
struct EndpointDescription;
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

class ApiRequestHandler : public httpd::HttpRequestHandler
{
public:
    ApiRequestHandler(bridge::MixerManager& mixerManager, transport::SslDtls& sslDtls);
    httpd::Response onRequest(const httpd::Request& request) override;

private:
    bridge::MixerManager& _mixerManager;
    transport::SslDtls& _sslDtls;
    std::atomic<uint32_t> _lastAutoRequestId;

#if ENABLE_LEGACY_API
    std::unique_ptr<LegacyApiRequestHandler> _legacyApiRequestHandler;
#endif

    httpd::Response handleStats(const httpd::Request& request);
    httpd::Response handleAbout(const httpd::Request& request, const utils::StringTokenizer::Token& token);
    httpd::Response allocateConference(RequestLogger& requestLogger, const httpd::Request& request);
    httpd::Response allocateEndpoint(RequestLogger& requestLogger,
        const api::AllocateEndpoint& allocateChannel,
        const std::string& conferenceId,
        const std::string& endpointId);

    httpd::Response generateAllocateEndpointResponse(RequestLogger& requestLogger,
        const api::AllocateEndpoint& allocateChannel,
        Mixer& mixer,
        const std::string& conferenceId,
        const std::string& endpointId);

    httpd::Response configureEndpoint(RequestLogger& requestLogger,
        const api::EndpointDescription& endpointDescription,
        const std::string& conferenceId,
        const std::string& endpointId);

    void configureAudioEndpoint(const api::EndpointDescription& endpointDescription,
        Mixer& mixer,
        const std::string& endpointId);

    void configureVideoEndpoint(const api::EndpointDescription& endpointDescription,
        Mixer& mixer,
        const std::string& endpointId);

    void configureDataEndpoint(const api::EndpointDescription& endpointDescription,
        Mixer& mixer,
        const std::string& endpointId);

    httpd::Response reconfigureEndpoint(RequestLogger& requestLogger,
        const api::EndpointDescription& endpointDescription,
        const std::string& conferenceId,
        const std::string& endpointId);

    httpd::Response recordEndpoint(RequestLogger& requestLogger,
        const api::Recording& recording,
        const std::string& conferenceId);
};

} // namespace bridge
