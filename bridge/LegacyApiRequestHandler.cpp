#include "bridge/LegacyApiRequestHandler.h"
#include "bridge/AudioStreamDescription.h"
#include "bridge/DataStreamDescription.h"
#include "bridge/LegacyApiRequestHandlerHelpers.h"
#include "bridge/Mixer.h"
#include "bridge/MixerManager.h"
#include "bridge/RequestLogger.h"
#include "bridge/TransportDescription.h"
#include "bridge/VideoStreamDescription.h"
#include "bridge/engine/SsrcWhitelist.h"
#include "legacyapi/Generator.h"
#include "legacyapi/Helpers.h"
#include "legacyapi/Parser.h"
#include "legacyapi/PatchConferenceType.h"
#include "legacyapi/Validator.h"
#include "logger/Logger.h"
#include "nlohmann/json.hpp"
#include "transport/Transport.h"
#include "transport/dtls/SslDtls.h"
#include "transport/ice/IceCandidate.h"
#include "utils/CheckedCast.h"
#include "utils/StringTokenizer.h"

namespace
{

const uint32_t gatheringCompleteMaxWaitMs = 5000;
const uint32_t gatheringCompleteWaitMs = 100;

const bridge::RtpMap kEmptyRtpMap;

bool contentNameToType(const std::string& contentName, bridge::LegacyApiRequestHandler::ContentType& outStreamType)
{
    if (contentName.compare("audio") == 0)
    {
        outStreamType = bridge::LegacyApiRequestHandler::ContentType::Audio;
        return true;
    }
    else if (contentName.compare("video") == 0)
    {
        outStreamType = bridge::LegacyApiRequestHandler::ContentType::Video;
        return true;
    }
    else
    {
        return false;
    }
}

bool makeTransportType(const std::string& protocol, ice::TransportType& outTransportType)
{
    if (protocol.compare("udp") == 0)
    {
        outTransportType = ice::TransportType::UDP;
    }
    else if (protocol.compare("tcp") == 0)
    {
        outTransportType = ice::TransportType::TCP;
    }
    else if (protocol.compare("ssltcp") == 0)
    {
        outTransportType = ice::TransportType::SSLTCP;
    }
    else
    {
        return false;
    }

    return true;
}

bool getIceCandidates(const legacyapi::Transport& transport,
    std::vector<ice::IceCandidate>& outIceCandidates,
    httpd::StatusCode& outStatus)
{
    std::vector<ice::IceCandidate> iceCandidates;
    for (const auto& candidate : transport._candidates)
    {
        ice::TransportType transportType;
        if (!makeTransportType(candidate._protocol, transportType))
        {
            outStatus = httpd::StatusCode::BAD_REQUEST;
            return false;
        }

        if (candidate._type.compare("host") == 0)
        {
            iceCandidates.emplace_back(candidate._foundation.c_str(),
                candidate._component == 1 ? ice::IceComponent::RTP : ice::IceComponent::RTCP,
                transportType,
                candidate._priority,
                transport::SocketAddress::parse(candidate._ip, candidate._port),
                ice::IceCandidate::Type::HOST);
        }
        else if ((candidate._type.compare("srflx") == 0 || candidate._type.compare("relay") == 0) &&
            candidate._relAddr.isSet() && candidate._relPort.isSet())
        {
            iceCandidates.emplace_back(candidate._foundation.c_str(),
                candidate._component == 1 ? ice::IceComponent::RTP : ice::IceComponent::RTCP,
                transportType,
                candidate._priority,
                transport::SocketAddress::parse(candidate._ip, candidate._port),
                transport::SocketAddress::parse(candidate._relAddr.get(), candidate._relPort.get()),
                candidate._type.compare("srflx") == 0 ? ice::IceCandidate::Type::SRFLX
                                                      : ice::IceCandidate::Type::RELAY);
        }
    }

    outIceCandidates.swap(iceCandidates);
    return true;
}

legacyapi::Candidate iceCandidateToApi(const ice::IceCandidate& iceCandidate)
{
    legacyapi::Candidate candidate;
    candidate._generation = 0;
    candidate._component = iceCandidate.component == ice::IceComponent::RTP ? 1 : 2;

    switch (iceCandidate.transportType)
    {
    case ice::TransportType::UDP:
        candidate._protocol = "udp";
        break;
    case ice::TransportType::TCP:
        candidate._protocol = "tcp";
        break;
    case ice::TransportType::SSLTCP:
        candidate._protocol = "ssltcp";
        break;
    default:
        assert(false);
        break;
    }

    candidate._port = iceCandidate.address.getPort();
    candidate._ip = iceCandidate.address.ipToString();

    switch (iceCandidate.type)
    {
    case ice::IceCandidate::Type::HOST:
        candidate._type = "host";
        break;
    case ice::IceCandidate::Type::SRFLX:
        candidate._type = "srflx";
        candidate._relPort.set(iceCandidate.baseAddress.getPort());
        candidate._relAddr.set(iceCandidate.baseAddress.ipToString());
        break;
    case ice::IceCandidate::Type::PRFLX:
        candidate._type = "prflx";
        candidate._relPort.set(iceCandidate.baseAddress.getPort());
        candidate._relAddr.set(iceCandidate.baseAddress.ipToString());
        break;
    default:
        candidate._type = "unsupported";
        assert(false);
        break;
    }

    candidate._foundation = iceCandidate.getFoundation();
    candidate._priority = iceCandidate.priority;
    candidate._network = 1;

    return candidate;
}

bridge::SsrcWhitelist makeWhitelistedSsrcsArray(const legacyapi::Channel& channel)
{
    bridge::SsrcWhitelist ssrcWhitelist;

    if (channel._ssrcWhitelist.isSet())
    {
        ssrcWhitelist.enabled = true;
        ssrcWhitelist.numSsrcs = std::min(channel._ssrcWhitelist.get().size(), ssrcWhitelist.ssrcs.size());
        for (size_t i = 0; i < ssrcWhitelist.numSsrcs; ++i)
        {
            ssrcWhitelist.ssrcs[i] = channel._ssrcWhitelist.get()[i];
        }
    }

    return ssrcWhitelist;
}

} // namespace

namespace bridge
{

LegacyApiRequestHandler::LegacyApiRequestHandler(bridge::MixerManager& mixerManager, transport::SslDtls& sslDtls)
    : _mixerManager(mixerManager),
      _sslDtls(sslDtls),
      lastAutoRequestId(0)
{
}

httpd::Response LegacyApiRequestHandler::onRequest(const httpd::Request& request)
{
    try
    {
        if (request.method == httpd::Method::OPTIONS)
        {
            return httpd::Response(httpd::StatusCode::NO_CONTENT);
        }

        auto token = utils::StringTokenizer::tokenize(request.url.c_str(), request.url.length(), '/');

        if (!utils::StringTokenizer::isEqual(token, "colibri") || !token.next)
        {
            return httpd::Response(httpd::StatusCode::NOT_FOUND);
        }

        token = utils::StringTokenizer::tokenize(token.next, token.remainingLength, '/');
        if (utils::StringTokenizer::isEqual(token, "stats"))
        {
            return handleStats(request);
        }
        else if (utils::StringTokenizer::isEqual(token, "conferences") && !token.next)
        {
            return handleConferences(request);
        }
        else if (utils::StringTokenizer::isEqual(token, "conferences") && token.next)
        {
            token = utils::StringTokenizer::tokenize(token.next, token.remainingLength, '/');
            return patchConference(request, std::string(token.start, token.length));
        }
        else
        {
            return httpd::Response(httpd::StatusCode::NOT_FOUND);
        }
    }
    catch (...)
    {
        logger::error("Uncaught exception in onRequest", "RequestHandler");
        return httpd::Response(httpd::StatusCode::INTERNAL_SERVER_ERROR);
    }
}

httpd::Response LegacyApiRequestHandler::handleConferences(const httpd::Request& request)
{
    if (request.method == httpd::Method::GET)
    {
        RequestLogger requestLogger(request, lastAutoRequestId);

        nlohmann::json responseBodyJson = nlohmann::json::array();
        for (const auto& mixerId : _mixerManager.getMixerIds())
        {
            responseBodyJson.push_back(mixerId);
        }

        httpd::Response response(httpd::StatusCode::OK, responseBodyJson.dump(4));
        response.headers["Content-type"] = "text/json";
        requestLogger.setResponse(response);
        return response;
    }
    else if (request.method == httpd::Method::POST)
    {
        return createConference(request);
    }
    else
    {
        RequestLogger requestLogger(request, lastAutoRequestId);
        httpd::Response response(httpd::StatusCode::METHOD_NOT_ALLOWED);
        requestLogger.setResponse(response);
        return response;
    }
}

httpd::Response LegacyApiRequestHandler::createConference(const httpd::Request& request)
{
    RequestLogger requestLogger(request, lastAutoRequestId);
    try
    {
        const auto requestBody = request.body.build();
        const auto requestBodyJson = nlohmann::json::parse(requestBody);
        if (!requestBodyJson.is_object())
        {
            httpd::Response response(httpd::StatusCode::BAD_REQUEST);
            requestLogger.setResponse(response);
            return response;
        }

        const auto lastNItr = requestBodyJson.find("last-n");
        auto mixer = lastNItr != requestBodyJson.end() && lastNItr->is_number() && lastNItr->get<int32_t>() > 0
            ? _mixerManager.create(lastNItr->get<uint32_t>(), true)
            : _mixerManager.create(true);

        if (!mixer)
        {
            httpd::Response response(httpd::StatusCode::INTERNAL_SERVER_ERROR);
            requestLogger.setResponse(response);
            return response;
        }

        logger::info("Create conference %s, mixer %s",
            "RequestHandler",
            mixer->getId().c_str(),
            mixer->getLoggableId().c_str());
        legacyapi::Conference conference;
        conference._id = mixer->getId();

        const auto responseBody = legacyapi::Generator::generate(conference);
        httpd::Response response(httpd::StatusCode::OK, responseBody.dump(4));
        response.headers["Content-type"] = "text/json";
        requestLogger.setResponse(response);
        return response;
    }
    catch (nlohmann::detail::parse_error)
    {
        logger::warn("Error parsing json", "RequestHandler");
        httpd::Response response(httpd::StatusCode::BAD_REQUEST);
        requestLogger.setResponse(response);
        return response;
    }
    catch (std::exception)
    {
        logger::error("Exception in createConference", "RequestHandler");
        httpd::Response response(httpd::StatusCode::BAD_REQUEST);
        requestLogger.setResponse(response);
        return response;
    }
}

httpd::Response LegacyApiRequestHandler::patchConference(const httpd::Request& request, const std::string& conferenceId)
{
    RequestLogger requestLogger(request, lastAutoRequestId);

    if (request.method != httpd::Method::PATCH)
    {
        httpd::Response response(httpd::StatusCode::METHOD_NOT_ALLOWED);
        requestLogger.setResponse(response);
        return response;
    }

    try
    {
        Mixer* mixer;
        auto scopedMixerLock = _mixerManager.getMixer(conferenceId, mixer);
        assert(scopedMixerLock.owns_lock());
        if (!mixer)
        {
            httpd::Response response(httpd::StatusCode::NOT_FOUND);
            requestLogger.setResponse(response);
            return response;
        }

        const auto requestBody = request.body.build();
        const auto requestBodyJson = nlohmann::json::parse(requestBody);
        const auto conference = legacyapi::Parser::parse(requestBodyJson);
        if (!legacyapi::Validator::isValid(conference))
        {
            httpd::Response response(httpd::StatusCode::BAD_REQUEST);
            requestLogger.setResponse(response);
            return response;
        }

        const bool useBundling = legacyapi::Helpers::bundlingEnabled(conference);
        const bool enableRecording = legacyapi::Helpers::hasRecording(requestBodyJson);
        auto patchConferenceType = legacyapi::PatchConferenceType::Undefined;

        for (const auto& content : conference._contents)
        {
            if (content._name.compare("audio") == 0 || content._name.compare("video") == 0)
            {
                logger::debug("%s content", "RequestHandler", content._name.c_str());
                if (content._channels.empty())
                {
                    httpd::Response response(httpd::StatusCode::BAD_REQUEST);
                    requestLogger.setResponse(response);
                    return response;
                }

                const auto& channel = content._channels[0];
                if (!channel._id.isSet())
                {
                    logger::debug("Channels id was not set, channels should be allocated", "RequestHandler");

                    if (patchConferenceType != legacyapi::PatchConferenceType::Undefined &&
                        patchConferenceType != legacyapi::PatchConferenceType::AllocateChannels)
                    {
                        httpd::Response response(httpd::StatusCode::BAD_REQUEST);
                        requestLogger.setResponse(response);
                        return response;
                    }
                    patchConferenceType = legacyapi::PatchConferenceType::AllocateChannels;

                    const auto transport = legacyapi::Helpers::getTransport(conference, channel);
                    httpd::StatusCode status;
                    std::string channelId;
                    if (!allocateChannel(content._name,
                            conferenceId,
                            channel,
                            transport,
                            useBundling,
                            *mixer,
                            status,
                            channelId))
                    {
                        httpd::Response response(status);
                        requestLogger.setResponse(response);
                        return response;
                    }

                    if (channel._transport.isSet() || !conference._channelBundles.empty())
                    {
                        logger::debug("Configure after allocating channel, since transport is set", "RequestHandler");
                        if (!configureChannel(content._name, conferenceId, channel, channelId, *mixer, status))
                        {
                            httpd::Response response(status);
                            requestLogger.setResponse(response);
                            return response;
                        }
                    }
                }
                else
                {
                    logger::debug(
                        "Channel id was set. Channels should be configured, reconfigured or expired, channel id %s",
                        "RequestHandler",
                        channel._id.get().c_str());
                    if (patchConferenceType == legacyapi::PatchConferenceType::AllocateChannels)
                    {
                        httpd::Response response(httpd::StatusCode::BAD_REQUEST);
                        requestLogger.setResponse(response);
                        return response;
                    }

                    if (channel._expire.isSet() && channel._expire.get() == 0)
                    {
                        logger::debug("Expire value was zero, channels should be expired, channel id %s",
                            "RequestHandler",
                            channel._id.get().c_str());
                        if (patchConferenceType != legacyapi::PatchConferenceType::Undefined &&
                            patchConferenceType != legacyapi::PatchConferenceType::ExpireChannels)
                        {
                            httpd::Response response(httpd::StatusCode::BAD_REQUEST);
                            requestLogger.setResponse(response);
                            return response;
                        }
                        patchConferenceType = legacyapi::PatchConferenceType::ExpireChannels;

                        httpd::StatusCode status;
                        if (!expireChannel(content._name, conferenceId, channel, *mixer, status))
                        {
                            httpd::Response response(status);
                            requestLogger.setResponse(response);
                            return response;
                        }
                    }
                    else
                    {
                        logger::debug("Expire value was not zero, channel should not be expired, channel id %s",
                            "RequestHandler",
                            channel._id.get().c_str());

                        if (content._name.compare("audio") == 0 &&
                            mixer->isAudioStreamConfigured(channel._endpoint.get()))
                        {
                            logger::debug("Channel is already configured, reconfigure, channel id %s",
                                "RequestHandler",
                                channel._id.get().c_str());
                            if (patchConferenceType != legacyapi::PatchConferenceType::Undefined &&
                                patchConferenceType != legacyapi::PatchConferenceType::ReconfigureChannels)
                            {
                                httpd::Response response(httpd::StatusCode::BAD_REQUEST);
                                requestLogger.setResponse(response);
                                return response;
                            }
                            patchConferenceType = legacyapi::PatchConferenceType::ReconfigureChannels;

                            httpd::StatusCode status;
                            if (!reconfigureAudioChannel(conferenceId, channel, *mixer, status))
                            {
                                httpd::Response response(status);
                                requestLogger.setResponse(response);
                                return response;
                            }
                        }
                        else if (content._name.compare("video") == 0 &&
                            mixer->isVideoStreamConfigured(channel._endpoint.get()))
                        {
                            logger::debug("Channel is already configured, reconfigure, channel id %s",
                                "RequestHandler",
                                channel._id.get().c_str());

                            if (patchConferenceType != legacyapi::PatchConferenceType::Undefined &&
                                patchConferenceType != legacyapi::PatchConferenceType::ReconfigureChannels)
                            {
                                httpd::Response response(httpd::StatusCode::BAD_REQUEST);
                                requestLogger.setResponse(response);
                                return response;
                            }
                            patchConferenceType = legacyapi::PatchConferenceType::ReconfigureChannels;

                            httpd::StatusCode status;
                            if (!reconfigureVideoChannel(conferenceId, channel, *mixer, status))
                            {
                                httpd::Response response(status);
                                requestLogger.setResponse(response);
                                return response;
                            }
                        }
                        else
                        {
                            logger::debug("Channel should be configured, channel id %s",
                                "RequestHandler",
                                channel._id.get().c_str());
                            if (patchConferenceType != legacyapi::PatchConferenceType::Undefined &&
                                patchConferenceType != legacyapi::PatchConferenceType::ConfigureChannels)
                            {
                                httpd::Response response(httpd::StatusCode::BAD_REQUEST);
                                requestLogger.setResponse(response);
                                return response;
                            }
                            patchConferenceType = legacyapi::PatchConferenceType::ConfigureChannels;

                            httpd::StatusCode status;
                            if (!configureChannel(content._name,
                                    conferenceId,
                                    channel,
                                    channel._id.get(),
                                    *mixer,
                                    status))
                            {
                                httpd::Response response(status);
                                requestLogger.setResponse(response);
                                return response;
                            }
                        }
                    }
                }
            }
            else
            {
                logger::debug("Data content (sctp connection)", "RequestHandler");
                if (content._sctpConnections.empty())
                {
                    httpd::Response response(httpd::StatusCode::BAD_REQUEST);
                    requestLogger.setResponse(response);
                    return response;
                }

                const auto& sctpConnection = content._sctpConnections[0];
                if (!sctpConnection._id.isSet())
                {
                    logger::debug("No sctpConnection id means an sctpConnection should be allocated", "RequestHandler");
                    if (patchConferenceType != legacyapi::PatchConferenceType::Undefined &&
                        patchConferenceType != legacyapi::PatchConferenceType::AllocateChannels)
                    {
                        httpd::Response response(httpd::StatusCode::BAD_REQUEST);
                        requestLogger.setResponse(response);
                        return response;
                    }
                    patchConferenceType = legacyapi::PatchConferenceType::AllocateChannels;

                    if (!useBundling)
                    {
                        httpd::Response response(httpd::StatusCode::BAD_REQUEST);
                        requestLogger.setResponse(response);
                        return response;
                    }

                    const auto transport = legacyapi::Helpers::getTransport(conference, sctpConnection);
                    httpd::StatusCode status;
                    if (!allocateSctpConnection(conferenceId, sctpConnection, transport, *mixer, status))
                    {
                        httpd::Response response(status);
                        requestLogger.setResponse(response);
                        return response;
                    }
                }
                else
                {
                    logger::debug("SctpConnection id means an sctpConnection should be configured or expired, "
                                  "sctpConnection id %s",
                        "RequestHandler",
                        sctpConnection._id.get().c_str());

                    if (sctpConnection._expire.isSet() && sctpConnection._expire.get() == 0)
                    {
                        logger::debug("Expire value was zero, sctpConnection should be expired, sctpConnection id %s",
                            "RequestHandler",
                            sctpConnection._id.get().c_str());

                        if (patchConferenceType != legacyapi::PatchConferenceType::Undefined &&
                            patchConferenceType != legacyapi::PatchConferenceType::ExpireChannels)
                        {
                            return httpd::Response(httpd::StatusCode::BAD_REQUEST);
                        }
                        patchConferenceType = legacyapi::PatchConferenceType::ExpireChannels;

                        if (!mixer->removeDataStreamId(sctpConnection._id.get()))
                        {
                            httpd::Response response(httpd::StatusCode::BAD_REQUEST);
                            requestLogger.setResponse(response);
                            return response;
                        }
                    }
                    else
                    {
                        logger::debug(
                            "Expire value was not zero, sctpConnection should not be expired, sctpConnection id %s",
                            "RequestHandler",
                            sctpConnection._id.get().c_str());

                        if (!mixer->isDataStreamConfigured(sctpConnection._endpoint.get()))
                        {
                            logger::debug("SctpConnection should be configured, sctpConnection id %s",
                                "RequestHandler",
                                sctpConnection._id.get().c_str());

                            if (patchConferenceType != legacyapi::PatchConferenceType::Undefined &&
                                patchConferenceType != legacyapi::PatchConferenceType::ConfigureChannels)
                            {
                                httpd::Response response(httpd::StatusCode::BAD_REQUEST);
                                requestLogger.setResponse(response);
                                return response;
                            }
                            patchConferenceType = legacyapi::PatchConferenceType::ConfigureChannels;

                            httpd::StatusCode status;
                            if (!configureSctpConnection(conferenceId, sctpConnection, *mixer, status))
                            {
                                httpd::Response response(status);
                                requestLogger.setResponse(response);
                                return response;
                            }
                        }
                    }
                }
            }
        }

        // If we allocated or configured channels, configure bundle transport if needed
        if (useBundling && !conference._channelBundles.empty() &&
            (patchConferenceType == legacyapi::PatchConferenceType::AllocateChannels ||
                patchConferenceType == legacyapi::PatchConferenceType::ConfigureChannels))
        {
            logger::debug("Configure bundle transport", "RequestHandler");

            auto& transport = conference._channelBundles[0]._transport;
            auto& endpointId = conference._channelBundles[0]._id;
            if (legacyapi::Helpers::iceEnabled(transport) && transport._ufrag.isSet() && transport._pwd.isSet())
            {
                std::vector<ice::IceCandidate> iceCandidates;
                httpd::StatusCode status;
                if (!getIceCandidates(transport, iceCandidates, status))
                {
                    httpd::Response response(status);
                    requestLogger.setResponse(response);
                    return response;
                }

                if (!mixer->configureBundleTransportIce(endpointId,
                        std::make_pair(transport._ufrag.get(), transport._pwd.get()),
                        iceCandidates))
                {
                    httpd::Response response(httpd::StatusCode::INTERNAL_SERVER_ERROR);
                    requestLogger.setResponse(response);
                    return response;
                }
            }

            const auto& fingerprint = transport._fingerprints[0];
            const bool isRemoteSideDtlsClient = fingerprint._setup.compare("active") == 0;

            if (!mixer->configureBundleTransportDtls(endpointId,
                    fingerprint._hash,
                    fingerprint._fingerprint,
                    !isRemoteSideDtlsClient))
            {
                httpd::Response response(httpd::StatusCode::INTERNAL_SERVER_ERROR);
                requestLogger.setResponse(response);
                return response;
            }

            if (!mixer->startBundleTransport(endpointId))
            {
                httpd::Response response(httpd::StatusCode::INTERNAL_SERVER_ERROR);
                requestLogger.setResponse(response);
                return response;
            }
        }

        // Create response for expired channels
        if (patchConferenceType == legacyapi::PatchConferenceType::ExpireChannels)
        {
            legacyapi::Conference responseData;
            responseData._id = conferenceId;

            const auto responseBody = legacyapi::Generator::generate(responseData);
            auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
            response.headers["Content-type"] = "text/json";
            logger::debug("PATCH expired response %s", "RequestHandler", response.body.c_str());
            requestLogger.setResponse(response);
            return response;
        }

        if (enableRecording && conference._recording.isSet())
        {
            auto recording = conference._recording.get();
            processRecording(*mixer, conferenceId, recording);

            if (patchConferenceType == legacyapi::PatchConferenceType::Undefined)
            {
                legacyapi::Conference responseData;
                responseData._id = conferenceId;
                const auto responseBody = legacyapi::Generator::generate(responseData);
                auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
                response.headers["Content-type"] = "text/json";
                logger::debug("PATCH response %s", "RequestHandler", response.body.c_str());
                requestLogger.setResponse(response);
                return response;
            }
        }

        assert(patchConferenceType != legacyapi::PatchConferenceType::Undefined);
        return generatePatchConferenceResponse(conference,
            conferenceId,
            useBundling,
            requestLogger,
            useBundling,
            *mixer);
    }
    catch (nlohmann::detail::parse_error parseError)
    {
        logger::warn("Error parsing json", "RequestHandler");
        httpd::Response response(httpd::StatusCode::BAD_REQUEST);
        requestLogger.setResponse(response);
        return response;
    }
    catch (std::exception exception)
    {
        logger::error("Exception in patchConference", "RequestHandler");
        httpd::Response response(httpd::StatusCode::BAD_REQUEST);
        requestLogger.setResponse(response);
        return response;
    }
}

httpd::Response LegacyApiRequestHandler::generatePatchConferenceResponse(const legacyapi::Conference& conference,
    const std::string& conferenceId,
    const bool useBundling,
    RequestLogger& requestLogger,
    const bool enableDtls,
    Mixer& mixer)
{
    legacyapi::Conference responseData(conference);
    responseData._id = conferenceId;

    // Describe bundle transport
    if (useBundling)
    {
        const auto channelBundleId = legacyapi::Helpers::getChannelBundleId(conference);
        if (!channelBundleId)
        {
            httpd::Response response(httpd::StatusCode::BAD_REQUEST);
            requestLogger.setResponse(response);
            return response;
        }

        responseData._channelBundles.clear();

        legacyapi::ChannelBundle channelBundle;
        channelBundle._id = *channelBundleId;

        TransportDescription transportDescription;
        if (!mixer.getTransportBundleDescription(channelBundle._id, transportDescription))
        {
            httpd::Response response(httpd::StatusCode::BAD_REQUEST);
            requestLogger.setResponse(response);
            return response;
        }

        if (transportDescription.ice.isSet())
        {
            const auto& transportDescriptionIce = transportDescription.ice.get();
            channelBundle._transport._ufrag.set(transportDescriptionIce.iceCredentials.first);
            channelBundle._transport._pwd.set(transportDescriptionIce.iceCredentials.second);
            channelBundle._transport._candidates.clear();
            for (const auto& iceCandidate : transportDescriptionIce.iceCandidates)
            {
                if (iceCandidate.type != ice::IceCandidate::Type::PRFLX)
                {
                    channelBundle._transport._candidates.emplace_back(iceCandidateToApi(iceCandidate));
                }
            }

            channelBundle._transport._rtcpMux = true;

            channelBundle._transport._fingerprints.clear();
            legacyapi::Fingerprint fingerprint;
            fingerprint._hash = "sha-256";
            fingerprint._fingerprint = _sslDtls.getLocalFingerprint();
            fingerprint._setup = "actpass";
            channelBundle._transport._fingerprints.push_back(fingerprint);
        }
        else
        {
            logger::warn("Bundling without ice not supported, conference %s",
                "RequestHandler",
                responseData._id.c_str());
            httpd::Response response(httpd::StatusCode::BAD_REQUEST);
            requestLogger.setResponse(response);
            return response;
        }
        responseData._channelBundles.emplace_back(channelBundle);
    }

    // Describe audio, video and data streams
    for (auto& content : responseData._contents)
    {
        if (content._name.compare("audio") == 0 || content._name.compare("video") == 0)
        {
            auto& channel = content._channels[0];

            TransportDescription transportDescription;
            if (content._name.compare("audio") == 0)
            {
                AudioStreamDescription streamDescription;
                if (!mixer.getAudioStreamDescription(channel._endpoint.get(), streamDescription) ||
                    !mixer.getAudioStreamTransportDescription(channel._endpoint.get(), transportDescription))
                {
                    httpd::Response response(httpd::StatusCode::BAD_REQUEST);
                    requestLogger.setResponse(response);
                    return response;
                }

                channel._sources = streamDescription.ssrcs;
                channel._id.set(streamDescription.id);
            }
            else
            {
                channel._ssrcGroups.clear();
                VideoStreamDescription streamDescription;
                if (!mixer.getVideoStreamDescription(channel._endpoint.get(), streamDescription) ||
                    !mixer.getVideoStreamTransportDescription(channel._endpoint.get(), transportDescription))
                {
                    httpd::Response response(httpd::StatusCode::BAD_REQUEST);
                    requestLogger.setResponse(response);
                    return response;
                }

                channel._sources = streamDescription.getSsrcs();
                channel._id.set(streamDescription.id);

                if (!streamDescription.sources.empty())
                {
                    for (auto level : streamDescription.sources)
                    {
                        legacyapi::SsrcGroup group;
                        group._sources.push_back(level.main);
                        group._sources.push_back(level.feedback);
                        group._semantics = "FID";
                        channel._ssrcGroups.push_back(group);
                    }

                    legacyapi::SsrcAttribute ssrcAttribute;
                    ssrcAttribute._sources.push_back(streamDescription.sources[0].main);
                    ssrcAttribute._content = "slides";
                    channel._ssrcAttributes.push_back(ssrcAttribute);
                }
            }

            if (channel._transport.isSet())
            {
                legacyapi::Transport channelTransport;

                if (legacyapi::Helpers::iceEnabled(channel._transport.get()) && transportDescription.ice.isSet())
                {
                    const auto& transportDescriptionIce = transportDescription.ice.get();
                    channelTransport._ufrag.set(transportDescriptionIce.iceCredentials.first);
                    channelTransport._pwd.set(transportDescriptionIce.iceCredentials.second);
                    channelTransport._candidates.clear();
                    for (const auto& iceCandidate : transportDescriptionIce.iceCandidates)
                    {
                        if (iceCandidate.type != ice::IceCandidate::Type::PRFLX)
                        {
                            channelTransport._candidates.emplace_back(iceCandidateToApi(iceCandidate));
                        }
                    }

                    channelTransport._rtcpMux = true;
                    channelTransport._xmlns.set("urn:xmpp:jingle:transports:ice-udp:1");
                }
                else
                {
                    if (transportDescription.localPeer.isSet() && !transportDescription.localPeer.get().empty())
                    {
                        legacyapi::Connection connection;
                        connection._ip = transportDescription.localPeer.get().ipToString();
                        connection._port = transportDescription.localPeer.get().getPort();
                        channelTransport._connection.set(connection);
                    }

                    channelTransport._rtcpMux = false;
                    channelTransport._xmlns.set("urn:xmpp:jingle:transports:raw-udp:1");
                }

                if (transportDescription.srtpMode == srtp::Mode::DTLS)
                {
                    channelTransport._fingerprints.clear();
                    legacyapi::Fingerprint fingerprint;
                    fingerprint._hash = "sha-256";
                    fingerprint._fingerprint = _sslDtls.getLocalFingerprint();
                    fingerprint._setup = "active";
                    channelTransport._fingerprints.push_back(fingerprint);
                }

                channel._transport.set(channelTransport);
            }
        }
        else
        {
            auto& sctpConnection = content._sctpConnections[0];
            DataStreamDescription streamDescription;
            if (!mixer.getDataStreamDescription(sctpConnection._endpoint.get(), streamDescription))
            {
                httpd::Response response(httpd::StatusCode::BAD_REQUEST);
                requestLogger.setResponse(response);
                return response;
            }

            sctpConnection._id.set(streamDescription.id);
            sctpConnection._port = streamDescription.sctpPort.isSet()
                ? utils::Optional<std::string>(std::to_string(streamDescription.sctpPort.get()))
                : utils::Optional<std::string>();
        }
    }

    const auto responseBody = legacyapi::Generator::generate(responseData);
    auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
    response.headers["Content-type"] = "text/json";
    logger::debug("PATCH response %s", "RequestHandler", response.body.c_str());
    requestLogger.setResponse(response);
    return response;
}

bool LegacyApiRequestHandler::allocateChannel(const std::string& contentName,
    const std::string& conferenceId,
    const legacyapi::Channel& channel,
    const legacyapi::Transport* transport,
    const bool useBundling,
    Mixer& mixer,
    httpd::StatusCode& outStatus,
    std::string& outChannelId)
{
    std::string channelId;
    if (!channel._endpoint.isSet() ||
        (channel._channelBundleId.isSet() && channel._endpoint.get().compare(channel._channelBundleId.get()) != 0))
    {
        outStatus = httpd::StatusCode::BAD_REQUEST;
        return false;
    }

    const auto& endpointId = channel._endpoint.get();

    ContentType contentType;
    if (!contentNameToType(contentName, contentType))
    {
        outStatus = httpd::StatusCode::BAD_REQUEST;
        return false;
    }

    utils::Optional<ice::IceRole> iceRole;
    if (!transport || !transport->_xmlns.isSet() || legacyapi::Helpers::iceEnabled(*transport))
    {
        iceRole.set(ice::IceRole::CONTROLLING);
    }

    if (useBundling)
    {
        mixer.addBundleTransportIfNeeded(endpointId, iceRole.get());
        if (contentType == ContentType::Audio)
        {
            if (!mixer.addBundledAudioStream(channelId, endpointId, channel.getMediaMode()))
            {
                outStatus = httpd::StatusCode::BAD_REQUEST;
                return false;
            }
        }
        else if (contentType == ContentType::Video)
        {
            if (!mixer.addBundledVideoStream(channelId, endpointId, channel.isRelayTypeRewrite()))
            {
                outStatus = httpd::StatusCode::BAD_REQUEST;
                return false;
            }
        }
    }
    else
    {
        if (contentType == ContentType::Audio)
        {
            if (!mixer.addAudioStream(channelId, endpointId, iceRole, channel.getMediaMode()))
            {
                outStatus = httpd::StatusCode::BAD_REQUEST;
                return false;
            }
        }
        else if (contentType == ContentType::Video)
        {
            if (!mixer.addVideoStream(channelId, endpointId, iceRole, channel.isRelayTypeRewrite()))
            {
                outStatus = httpd::StatusCode::BAD_REQUEST;
                return false;
            }
        }
    }

    uint32_t totalSleepTimeMs = 0;

    if (contentType == ContentType::Audio)
    {
        while (!mixer.isAudioStreamGatheringComplete(endpointId) && totalSleepTimeMs < gatheringCompleteMaxWaitMs)
        {
            totalSleepTimeMs += gatheringCompleteWaitMs;
            usleep(gatheringCompleteWaitMs * 1000);
        }
    }
    else if (contentType == ContentType::Video)
    {
        while (!mixer.isVideoStreamGatheringComplete(endpointId) && totalSleepTimeMs < gatheringCompleteMaxWaitMs)
        {
            totalSleepTimeMs += gatheringCompleteWaitMs;
            usleep(gatheringCompleteWaitMs * 1000);
        }
    }

    if (totalSleepTimeMs >= gatheringCompleteWaitMs)
    {
        logger::error("Create stream id %s, endpointId %s, type %s, mixer %s, gathering did not complete in time",
            "RequestHandler",
            channelId.c_str(),
            endpointId.c_str(),
            contentName.c_str(),
            conferenceId.c_str());

        if (contentType == ContentType::Audio)
        {
            mixer.removeAudioStream(endpointId);
        }
        else if (contentType == ContentType::Video)
        {
            mixer.removeVideoStream(endpointId);
        }
        outStatus = httpd::StatusCode::INTERNAL_SERVER_ERROR;
        return false;
    }

    outChannelId = channelId;
    return true;
}

bool LegacyApiRequestHandler::allocateSctpConnection(const std::string& conferenceId,
    const legacyapi::SctpConnection& sctpConnection,
    const legacyapi::Transport* transport,
    Mixer& mixer,
    httpd::StatusCode& outStatus)
{
    std::string streamId;
    if (!sctpConnection._endpoint.isSet() ||
        (!sctpConnection._channelBundleId.isSet() &&
            sctpConnection._endpoint.get().compare(sctpConnection._channelBundleId.get()) != 0))
    {
        outStatus = httpd::StatusCode::BAD_REQUEST;
        return false;
    }

    const auto& endpointId = sctpConnection._endpoint.get();

    utils::Optional<ice::IceRole> iceRole;
    if (!transport || !transport->_xmlns.isSet() || legacyapi::Helpers::iceEnabled(*transport))
    {
        iceRole.set(ice::IceRole::CONTROLLING);
    }
    mixer.addBundleTransportIfNeeded(endpointId, iceRole.get());
    mixer.addBundledDataStream(streamId, endpointId);

    uint32_t totalSleepTimeMs = 0;
    while (!mixer.isDataStreamGatheringComplete(endpointId) && totalSleepTimeMs < gatheringCompleteMaxWaitMs)
    {
        totalSleepTimeMs += gatheringCompleteWaitMs;
        usleep(gatheringCompleteWaitMs * 1000);
    }

    if (totalSleepTimeMs >= gatheringCompleteWaitMs)
    {
        logger::error("Create stream id %s, endpointId %s, type data, mixer %s, gathering did not complete in time",
            "RequestHandler",
            streamId.c_str(),
            endpointId.c_str(),
            conferenceId.c_str());

        mixer.removeDataStream(endpointId);
        outStatus = httpd::StatusCode::INTERNAL_SERVER_ERROR;
        return false;
    }

    return true;
}

bool LegacyApiRequestHandler::configureChannel(const std::string& contentName,
    const std::string& conferenceId,
    const legacyapi::Channel& channel,
    const std::string& channelId,
    Mixer& mixer,
    httpd::StatusCode& outStatus)
{
    ContentType contentType;
    if (!channel._endpoint.isSet() || !contentNameToType(contentName, contentType))
    {
        outStatus = httpd::StatusCode::BAD_REQUEST;
        return false;
    }

    const auto& endpointId = channel._endpoint.get();

    if (!channel._payloadTypes.empty() && channel._direction.isSet())
    {
        const auto rtpMaps = LegacyApiRequestHandlerHelpers::makeRtpMaps(channel);
        if (rtpMaps.empty())
        {
            outStatus = httpd::StatusCode::BAD_REQUEST;
            return false;
        }

        if (contentType == ContentType::Audio)
        {
            utils::Optional<uint32_t> remoteSsrc;
            if (!channel._sources.empty())
            {
                remoteSsrc.set(channel._sources.front());
            }

            std::vector<uint32_t> noNeighbours;
            if (!mixer.configureAudioStream(endpointId, rtpMaps.front(), kEmptyRtpMap, remoteSsrc, noNeighbours))
            {
                outStatus = httpd::StatusCode::BAD_REQUEST;
                return false;
            }
        }
        else if (contentType == ContentType::Video)
        {
            const auto feedbackRtpMap = rtpMaps.size() > 1 ? rtpMaps[1] : RtpMap();
            auto simulcastStreams = LegacyApiRequestHandlerHelpers::makeSimulcastStreams(channel);
            if (simulcastStreams.size() > 2)
            {
                outStatus = httpd::StatusCode::BAD_REQUEST;
                return false;
            }

            utils::Optional<SimulcastStream> secondarySimulcastStream;
            if (simulcastStreams.size() == 2)
            {
                secondarySimulcastStream.set(simulcastStreams[1]);
            }
            else if (simulcastStreams.empty())
            {
                SimulcastStream emptySimulcastStream;
                memset(&emptySimulcastStream, 0, sizeof(SimulcastStream));
                simulcastStreams.emplace_back(emptySimulcastStream);
            }

            const auto ssrcWhitelist = makeWhitelistedSsrcsArray(channel);

            if (!mixer.configureVideoStream(endpointId,
                    rtpMaps.front(),
                    feedbackRtpMap,
                    simulcastStreams[0],
                    secondarySimulcastStream,
                    ssrcWhitelist))
            {
                outStatus = httpd::StatusCode::BAD_REQUEST;
                return false;
            }
        }
    }

    if (channel._transport.isSet())
    {
        const auto& channelTransport = channel._transport.get();
        bool configureTransport = false;

        if (legacyapi::Helpers::iceEnabled(channelTransport) && channelTransport._ufrag.isSet() &&
            channelTransport._pwd.isSet())
        {
            configureTransport = true;

            std::vector<ice::IceCandidate> iceCandidates;
            if (!getIceCandidates(channelTransport, iceCandidates, outStatus))
            {
                return false;
            }
            const auto credentials = std::make_pair(channelTransport._ufrag.get(), channel._transport.get()._pwd.get());

            if ((contentType == ContentType::Audio &&
                    !mixer.configureAudioStreamTransportIce(endpointId, credentials, iceCandidates)) ||
                (contentType == ContentType::Video &&
                    !mixer.configureVideoStreamTransportIce(endpointId, credentials, iceCandidates)))
            {
                outStatus = httpd::StatusCode::INTERNAL_SERVER_ERROR;
                return false;
            }
        }
        else if (channelTransport._connection.isSet())
        {
            configureTransport = true;

            const auto& connection = channelTransport._connection.get();
            const auto remotePeer = transport::SocketAddress::parse(connection._ip, connection._port);
            if ((contentType == ContentType::Audio &&
                    !mixer.configureAudioStreamTransportConnection(endpointId, remotePeer)) ||
                (contentType == ContentType::Video &&
                    !mixer.configureVideoStreamTransportConnection(endpointId, remotePeer)))
            {
                outStatus = httpd::StatusCode::INTERNAL_SERVER_ERROR;
                return false;
            }
        }

        if (configureTransport)
        {
            if (!channelTransport._fingerprints.empty())
            {
                const auto& fingerprint = channelTransport._fingerprints[0];
                const bool isRemoteSideDtlsClient = fingerprint._setup.compare("active") == 0;

                if ((contentType == ContentType::Audio &&
                        !mixer.configureAudioStreamTransportDtls(endpointId,
                            fingerprint._hash,
                            fingerprint._fingerprint,
                            !isRemoteSideDtlsClient)) ||
                    (contentType == ContentType::Video &&
                        !mixer.configureVideoStreamTransportDtls(endpointId,
                            fingerprint._hash,
                            fingerprint._fingerprint,
                            !isRemoteSideDtlsClient)))
                {
                    outStatus = httpd::StatusCode::INTERNAL_SERVER_ERROR;
                    return false;
                }
            }
            else
            {
                if ((contentType == ContentType::Audio &&
                        !mixer.configureAudioStreamTransportDisableSrtp(endpointId)) ||
                    (contentType == ContentType::Video && !mixer.configureVideoStreamTransportDisableSrtp(endpointId)))
                {
                    outStatus = httpd::StatusCode::INTERNAL_SERVER_ERROR;
                    return false;
                }
            }

            if ((contentType == ContentType::Audio &&
                    (!mixer.addAudioStreamToEngine(endpointId) || !mixer.startAudioStreamTransport(endpointId))) ||
                (contentType == ContentType::Video &&
                    (!mixer.addVideoStreamToEngine(endpointId) || !mixer.startVideoStreamTransport(endpointId))))
            {
                outStatus = httpd::StatusCode::INTERNAL_SERVER_ERROR;
                return false;
            }
        }
    }
    else
    {
        if ((contentType == ContentType::Audio && !mixer.addAudioStreamToEngine(endpointId)) ||
            (contentType == ContentType::Video && !mixer.addVideoStreamToEngine(endpointId)))
        {
            outStatus = httpd::StatusCode::INTERNAL_SERVER_ERROR;
            return false;
        }
    }

    return true;
}

bool LegacyApiRequestHandler::configureSctpConnection(const std::string& conferenceId,
    const legacyapi::SctpConnection& sctpConnection,
    Mixer& mixer,
    httpd::StatusCode& outStatus)
{
    if (!sctpConnection._endpoint.isSet() || !sctpConnection._port.isSet())
    {
        outStatus = httpd::StatusCode::BAD_REQUEST;
        return false;
    }

    const auto& endpointId = sctpConnection._endpoint.get();

    mixer.configureDataStream(endpointId, std::stoul(sctpConnection._port.get()));
    mixer.addDataStreamToEngine(endpointId);
    return true;
}

bool LegacyApiRequestHandler::reconfigureAudioChannel(const std::string& conferenceId,
    const legacyapi::Channel& channel,
    Mixer& mixer,
    httpd::StatusCode& outStatus)
{
    if (!channel._direction.isSet() || !channel._endpoint.isSet())
    {
        outStatus = httpd::StatusCode::BAD_REQUEST;
        return false;
    }

    const auto& endpointId = channel._endpoint.get();

    utils::Optional<uint32_t> remoteSsrc;
    if (!channel._sources.empty())
    {
        remoteSsrc.set(channel._sources.front());
    }

    if (!mixer.reconfigureAudioStream(endpointId, remoteSsrc))
    {
        outStatus = httpd::StatusCode::BAD_REQUEST;
        return false;
    }

    return true;
}

bool LegacyApiRequestHandler::reconfigureVideoChannel(const std::string& conferenceId,
    const legacyapi::Channel& channel,
    Mixer& mixer,
    httpd::StatusCode& outStatus)
{
    if (!channel._direction.isSet() || !channel._endpoint.isSet())
    {
        outStatus = httpd::StatusCode::BAD_REQUEST;
        return false;
    }

    const auto& endpointId = channel._endpoint.get();

    auto simulcastStreams = LegacyApiRequestHandlerHelpers::makeSimulcastStreams(channel);
    if (simulcastStreams.size() > 2)
    {
        outStatus = httpd::StatusCode::BAD_REQUEST;
        return false;
    }

    if (simulcastStreams.empty())
    {
        SimulcastStream emptySimulcastStream;
        memset(&emptySimulcastStream, 0, sizeof(SimulcastStream));
        simulcastStreams.emplace_back(emptySimulcastStream);
    }

    utils::Optional<SimulcastStream> secondarySimulcastStream;
    if (simulcastStreams.size() == 2)
    {
        secondarySimulcastStream.set(simulcastStreams[1]);
    }

    const auto ssrcWhitelist = makeWhitelistedSsrcsArray(channel);

    if (!mixer.reconfigureVideoStream(endpointId, simulcastStreams[0], secondarySimulcastStream, ssrcWhitelist))
    {
        outStatus = httpd::StatusCode::BAD_REQUEST;
        return false;
    }

    return true;
}

bool LegacyApiRequestHandler::expireChannel(const std::string& contentName,
    const std::string& conferenceId,
    const legacyapi::Channel& channel,
    Mixer& mixer,
    httpd::StatusCode& outStatus)
{
    ContentType contentType;
    if (!contentNameToType(contentName, contentType))
    {
        outStatus = httpd::StatusCode::BAD_REQUEST;
        return false;
    }

    if ((contentType == ContentType::Audio && !mixer.removeAudioStreamId(channel._id.get())) ||
        (contentType == ContentType::Video && !mixer.removeVideoStreamId(channel._id.get())))
    {
        outStatus = httpd::StatusCode::BAD_REQUEST;
        return false;
    }

    return true;
}

httpd::Response LegacyApiRequestHandler::handleStats(const httpd::Request& request)
{
    if (request.method != httpd::Method::GET)
    {
        return httpd::Response(httpd::StatusCode::METHOD_NOT_ALLOWED);
    }

    auto stats = _mixerManager.getStats();
    const auto statsDescription = stats.describe();
    httpd::Response response(httpd::StatusCode::OK, statsDescription);
    response.headers["Content-type"] = "text/json";
    return response;
}

bool LegacyApiRequestHandler::processRecording(Mixer& mixer,
    const std::string& conferenceId,
    const api::Recording& recording)
{
    const bool isRecordingStart =
        recording.isAudioEnabled || recording.isVideoEnabled || recording.isScreenshareEnabled;

    if (isRecordingStart)
    {
        RecordingDescription description;
        description.ownerId = recording.userId;
        description.recordingId = recording.recordingId;
        description.isAudioEnabled = recording.isAudioEnabled;
        description.isVideoEnabled = recording.isVideoEnabled;
        description.isScreenSharingEnabled = recording.isScreenshareEnabled;

        return mixer.addOrUpdateRecording(conferenceId, recording.channels, description);
    }
    else if (!recording.channels.empty())
    {
        return mixer.removeRecordingTransports(conferenceId, recording.channels);
    }
    else
    {
        return mixer.removeRecording(recording.recordingId);
    }
}

} // namespace bridge
