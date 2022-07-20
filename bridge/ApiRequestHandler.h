#pragma once

#include "httpd/HttpRequestHandler.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
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

    httpd::Response handleStats(const httpd::Request&);
    httpd::Response handleAbout(const httpd::Request&, const utils::StringTokenizer::Token&);
    httpd::Response getConferences(RequestLogger&);
    httpd::Response allocateConference(RequestLogger&, const httpd::Request&);
    httpd::Response allocateEndpoint(RequestLogger&,
        const api::AllocateEndpoint&,
        const std::string& conferenceId,
        const std::string& endpointId);
    httpd::Response processConferenceAction(RequestLogger&,
        const httpd::Request&,
        const utils::StringTokenizer::Token&);
    httpd::Response processBarbellRequest(RequestLogger&, const httpd::Request&, const utils::StringTokenizer::Token&);

    httpd::Response generateAllocateEndpointResponse(RequestLogger&,
        const api::AllocateEndpoint&,
        Mixer&,
        const std::string& conferenceId,
        const std::string& endpointId);

    httpd::Response configureEndpoint(RequestLogger&,
        const api::EndpointDescription&,
        const std::string& conferenceId,
        const std::string& endpointId);

    void configureAudioEndpoint(const api::EndpointDescription&, Mixer&, const std::string& endpointId);

    void configureVideoEndpoint(const api::EndpointDescription&, Mixer&, const std::string& endpointId);

    void configureDataEndpoint(const api::EndpointDescription&, Mixer&, const std::string& endpointId);

    httpd::Response reconfigureEndpoint(RequestLogger&,
        const api::EndpointDescription&,
        const std::string& conferenceId,
        const std::string& endpointId);

    httpd::Response recordEndpoint(RequestLogger&, const api::Recording& recording, const std::string& conferenceId);

    httpd::Response expireEndpoint(RequestLogger&, const std::string& conferenceId, const std::string& endpointId);

    httpd::Response allocateBarbell(RequestLogger&,
        bool iceControlling,
        const std::string& conferenceId,
        const std::string& barbellId);

    httpd::Response configureBarbell(RequestLogger&,
        const std::string& conferenceId,
        const std::string& barbellId,
        const api::EndpointDescription& barbellDescription);

    httpd::Response deleteBarbell(RequestLogger&, const std::string& conferenceId, const std::string& barbellId);

    httpd::Response generateBarbellResponse(RequestLogger&,
        Mixer&,
        const std::string& conferenceId,
        const std::string& barbellId,
        bool dtlsClient);

    httpd::Response getConferenceInfo(RequestLogger&, const utils::StringTokenizer::Token&);
    httpd::Response getEndpointInfo(RequestLogger&, const std::string& conferenceId, const std::string& endpointId);

    std::unique_lock<std::mutex> getConferenceMixer(const std::string& conferenceId, Mixer*&);
};

} // namespace bridge
