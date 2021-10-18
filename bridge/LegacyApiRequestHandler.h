#pragma once

#include "api/Recording.h"
#include "httpd/HttpRequestHandler.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace legacyapi
{
struct Conference;
struct Channel;
struct Transport;
struct SctpConnection;
} // namespace legacyapi

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

class LegacyApiRequestHandler : public httpd::HttpRequestHandler
{
public:
    enum class ContentType
    {
        Audio,
        Video
    };

    LegacyApiRequestHandler(bridge::MixerManager& mixerManager, transport::SslDtls& sslDtls);
    httpd::Response onRequest(const httpd::Request& request) override;

private:
    bridge::MixerManager& _mixerManager;
    transport::SslDtls& _sslDtls;
    std::atomic<uint32_t> lastAutoRequestId;

    httpd::Response handleConferences(const httpd::Request& request);
    httpd::Response createConference(const httpd::Request& request);
    httpd::Response patchConference(const httpd::Request& request, const std::string& conferenceId);
    httpd::Response generatePatchConferenceResponse(const legacyapi::Conference& conference,
        const std::string& conferenceId,
        const bool useBundling,
        RequestLogger& requestLogger,
        Mixer& mixer);

    bool allocateChannel(const std::string& contentName,
        const std::string& conferenceId,
        const legacyapi::Channel& channel,
        const legacyapi::Transport* transport,
        const bool useBundling,
        Mixer& mixer,
        httpd::StatusCode& outStatus,
        std::string& outChannelId);

    bool allocateSctpConnection(const std::string& conferenceId,
        const legacyapi::SctpConnection& sctpConnection,
        const legacyapi::Transport* transport,
        Mixer& mixer,
        httpd::StatusCode& outStatus);

    bool configureChannel(const std::string& contentName,
        const std::string& conferenceId,
        const legacyapi::Channel& channel,
        const std::string& channelId,
        Mixer& mixer,
        httpd::StatusCode& outStatus);

    bool configureSctpConnection(const std::string& conferenceId,
        const legacyapi::SctpConnection& sctpConnection,
        Mixer& mixer,
        httpd::StatusCode& outStatus);

    bool reconfigureAudioChannel(const std::string& conferenceId,
        const legacyapi::Channel& channel,
        Mixer& mixer,
        httpd::StatusCode& outStatus);

    bool reconfigureVideoChannel(const std::string& conferenceId,
        const legacyapi::Channel& channel,
        Mixer& mixer,
        httpd::StatusCode& outStatus);

    bool expireChannel(const std::string& contentName,
        const std::string& conferenceId,
        const legacyapi::Channel& channel,
        Mixer& mixer,
        httpd::StatusCode& outStatus);

    httpd::Response handleStats(const httpd::Request& request);

    bool processRecording(Mixer& mixer, const std::string& conferenceId, const api::Recording& recording);
};

} // namespace bridge
