#include "bridge/ApiRequestHandler.h"
#include "api/EndpointDescription.h"
#include "api/Generator.h"
#include "api/Parser.h"
#include "bridge/DataStreamDescription.h"
#include "bridge/Mixer.h"
#include "bridge/MixerManager.h"
#include "bridge/RequestLogger.h"
#include "bridge/StreamDescription.h"
#include "bridge/TransportDescription.h"
#include "bridge/engine/SsrcWhitelist.h"
#include "codec/Opus.h"
#include "codec/Vp8.h"
#include "httpd/RequestErrorException.h"
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

api::EndpointDescription::Candidate iceCandidateToApi(const ice::IceCandidate& iceCandidate)
{
    api::EndpointDescription::Candidate candidate;
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

void addDefaultAudioProperties(api::EndpointDescription::Audio& audioChannel)
{
    api::EndpointDescription::PayloadType opus;
    opus._id = codec::Opus::payloadType;
    opus._name = "opus";
    opus._clockRate = codec::Opus::sampleRate;
    opus._channels.set(codec::Opus::channelsPerFrame);
    opus._parameters.emplace_back("minptime", "10");
    opus._parameters.emplace_back("useinbandfec", "1");

    audioChannel._payloadType.set(opus);
    audioChannel._rtpHeaderExtensions.emplace_back(1, "urn:ietf:params:rtp-hdrext:ssrc-audio-level");
    audioChannel._rtpHeaderExtensions.emplace_back(3, "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time");
}

void addDefaultVideoProperties(api::EndpointDescription::Video& videoChannel)
{
    {
        api::EndpointDescription::PayloadType vp8;
        vp8._id = codec::Vp8::payloadType;
        vp8._name = "VP8";
        vp8._clockRate = codec::Vp8::sampleRate;
        vp8._rtcpFeedbacks.emplace_back("goog-remb", utils::Optional<std::string>());
        vp8._rtcpFeedbacks.emplace_back("nack", utils::Optional<std::string>());
        vp8._rtcpFeedbacks.emplace_back("nack", utils::Optional<std::string>("pli"));
        videoChannel._payloadTypes.push_back(vp8);
    }

    {
        api::EndpointDescription::PayloadType vp8Rtx;
        vp8Rtx._id = codec::Vp8::rtxPayloadType;
        vp8Rtx._name = "rtx";
        vp8Rtx._clockRate = codec::Vp8::sampleRate;
        vp8Rtx._parameters.emplace_back("apt", std::to_string(codec::Vp8::payloadType));
        videoChannel._payloadTypes.push_back(vp8Rtx);
    }

    videoChannel._rtpHeaderExtensions.emplace_back(3, "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time");
    videoChannel._rtpHeaderExtensions.emplace_back(4, "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id");
}

bridge::RtpMap makeRtpMap(const api::EndpointDescription::PayloadType& payloadType)
{
    bridge::RtpMap rtpMap;

    if (payloadType._name.compare("opus") == 0)
    {
        rtpMap = bridge::RtpMap(bridge::RtpMap::Format::OPUS,
            payloadType._id,
            payloadType._clockRate,
            payloadType._channels);
    }
    else if (payloadType._name.compare("VP8") == 0)
    {
        rtpMap = bridge::RtpMap(bridge::RtpMap::Format::VP8, payloadType._id, payloadType._clockRate);
    }
    else if (payloadType._name.compare("rtx") == 0)
    {
        rtpMap = bridge::RtpMap(bridge::RtpMap::Format::VP8RTX, codec::Vp8::rtxPayloadType, codec::Vp8::sampleRate);
    }
    else
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
    }

    for (const auto& parameter : payloadType._parameters)
    {
        rtpMap._parameters.emplace(parameter.first, parameter.second);
    }

    return rtpMap;
}

const api::EndpointDescription::SsrcGroup* findSimulcastGroup(const api::EndpointDescription::Video& video,
    const uint32_t ssrc)
{
    for (auto& ssrcGroup : video._ssrcGroups)
    {
        if (ssrcGroup._semantics.compare("SIM") == 0 && ssrcGroup._ssrcs.size() > 1)
        {
            const auto source = std::find(ssrcGroup._ssrcs.begin(), ssrcGroup._ssrcs.end(), ssrc);
            if (source != ssrcGroup._ssrcs.end())
            {
                return &ssrcGroup;
            }
        }
    }
    return nullptr;
}

const api::EndpointDescription::SsrcGroup* findFeedbackGroup(const api::EndpointDescription::Video& video,
    const uint32_t ssrc)
{
    for (auto& ssrcGroup : video._ssrcGroups)
    {
        if (ssrcGroup._semantics.compare("FID") == 0 && ssrcGroup._ssrcs.size() == 2 && ssrcGroup._ssrcs[0] == ssrc)
        {
            return &ssrcGroup;
        }
    }
    return nullptr;
}

std::vector<bridge::SimulcastStream> makeSimulcastStreams(const api::EndpointDescription::Video& video,
    const std::string& endpointId)
{
    std::vector<bridge::SimulcastStream> simulcastStreams;
    for (const auto sourcesSsrc : video._ssrcs)
    {
        auto simulcastGroup = findSimulcastGroup(video, sourcesSsrc);

        if (simulcastGroup)
        {
            assert(simulcastGroup->_ssrcs.size() > 1);
            const auto sources = simulcastGroup->_ssrcs;
            if (std::find(sources.begin() + 1, sources.end(), sourcesSsrc) != sources.end())
            {
                continue;
            }
        }

        if (simulcastGroup && sourcesSsrc == simulcastGroup->_ssrcs[0])
        {
            bridge::SimulcastStream simulcastStream{0};

            for (auto& ssrcAttribute : video._ssrcAttributes)
            {
                if (ssrcAttribute._content.compare(api::EndpointDescription::SsrcAttribute::slidesContent) == 0 &&
                    ssrcAttribute._ssrcs[0] == sourcesSsrc)
                {
                    simulcastStream._contentType = bridge::SimulcastStream::VideoContentType::SLIDES;
                }
            }

            for (auto simulcastSsrc : simulcastGroup->_ssrcs)
            {
                const auto feedbackGroup = findFeedbackGroup(video, simulcastSsrc);
                if (!feedbackGroup)
                {
                    continue;
                }

                simulcastStream._levels[simulcastStream._numLevels]._ssrc = simulcastSsrc;
                simulcastStream._levels[simulcastStream._numLevels]._feedbackSsrc = feedbackGroup->_ssrcs[1];
                ++simulcastStream._numLevels;

                logger::debug("Add simulcast level main ssrc %u feedback ssrc %u, content %s, endpointId %s",
                    "ApiRequestHandler",
                    simulcastSsrc,
                    feedbackGroup->_ssrcs[1],
                    toString(simulcastStream._contentType),
                    endpointId.c_str());
            }

            simulcastStreams.emplace_back(simulcastStream);
        }
        else
        {
            const auto feedbackGroup = findFeedbackGroup(video, sourcesSsrc);
            if (!feedbackGroup)
            {
                continue;
            }

            bridge::SimulcastStream simulcastStream{0};
            simulcastStream._numLevels = 1;

            simulcastStream._levels[0]._ssrc = sourcesSsrc;
            simulcastStream._levels[0]._feedbackSsrc = feedbackGroup->_ssrcs[1];

            for (auto& ssrcAttribute : video._ssrcAttributes)
            {
                if (ssrcAttribute._content.compare(api::EndpointDescription::SsrcAttribute::slidesContent) == 0 &&
                    ssrcAttribute._ssrcs[0] == sourcesSsrc)
                {
                    simulcastStream._contentType = bridge::SimulcastStream::VideoContentType::SLIDES;
                }
            }

            logger::debug("Add non-simulcast stream main ssrc %u feedback ssrc %u, content %s, endpointId %s",
                "ApiRequestHandler",
                sourcesSsrc,
                feedbackGroup->_ssrcs[1],
                toString(simulcastStream._contentType),
                endpointId.c_str());

            simulcastStreams.emplace_back(simulcastStream);
        }
    }

    if (simulcastStreams.size() > 2)
    {
        return std::vector<bridge::SimulcastStream>(simulcastStreams.end() - 2, simulcastStreams.end());
    }

    return simulcastStreams;
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

bridge::SsrcWhitelist makeWhitelistedSsrcsArray(const api::EndpointDescription::Video& video)
{
    bridge::SsrcWhitelist ssrcWhitelist = {false, 0, {0, 0}};

    if (video._ssrcWhitelist.isSet())
    {
        ssrcWhitelist._enabled = true;
        ssrcWhitelist._numSsrcs = std::min(video._ssrcWhitelist.get().size(), ssrcWhitelist._ssrcs.size());
        for (size_t i = 0; i < ssrcWhitelist._numSsrcs; ++i)
        {
            ssrcWhitelist._ssrcs[i] = video._ssrcWhitelist.get()[i];
        }
    }

    return ssrcWhitelist;
}

utils::Optional<uint8_t> findAbsSendTimeExtensionId(
    const std::vector<std::pair<uint32_t, std::string>>& rtpHeaderExtensions)
{
    for (const auto& rtpHeaderExtension : rtpHeaderExtensions)
    {
        if (rtpHeaderExtension.second.compare("http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time") == 0)
        {
            return utils::Optional<uint8_t>((utils::checkedCast<uint8_t>(rtpHeaderExtension.first)));
        }
    }
    return utils::Optional<uint8_t>();
}

std::pair<std::vector<ice::IceCandidate>, std::pair<std::string, std::string>> getIceCandidatesAndCredentials(
    const api::EndpointDescription::Transport& transport)
{
    const auto& ice = transport._ice.get();
    std::vector<ice::IceCandidate> candidates;

    for (const auto& candidate : ice._candidates)
    {
        ice::TransportType transportType;
        if (!makeTransportType(candidate._protocol, transportType))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
        }

        if (candidate._type.compare("host") == 0)
        {
            candidates.emplace_back(candidate._foundation.c_str(),
                candidate._component == 1 ? ice::IceComponent::RTP : ice::IceComponent::RTCP,
                transportType,
                candidate._priority,
                transport::SocketAddress::parse(candidate._ip, candidate._port),
                ice::IceCandidate::Type::HOST);
        }
        else if (candidate._type.compare("srflx") == 0 && candidate._relAddr.isSet() && candidate._relPort.isSet())
        {
            candidates.emplace_back(candidate._foundation.c_str(),
                candidate._component == 1 ? ice::IceComponent::RTP : ice::IceComponent::RTCP,
                transportType,
                candidate._priority,
                transport::SocketAddress::parse(candidate._ip, candidate._port),
                transport::SocketAddress::parse(candidate._relAddr.get(), candidate._relPort.get()),
                ice::IceCandidate::Type::SRFLX);
        }
    }

    return std::make_pair(candidates, std::make_pair(ice._ufrag, ice._pwd));
}

} // namespace

namespace bridge
{

ApiRequestHandler::ApiRequestHandler(bridge::MixerManager& mixerManager, transport::SslDtls& sslDtls)
    : _mixerManager(mixerManager),
      _sslDtls(sslDtls),
      _lastAutoRequestId(0)
#if ENABLE_LEGACY_API
      ,
      _legacyApiRequestHandler(std::make_unique<LegacyApiRequestHandler>(mixerManager, sslDtls))
#endif
{
}

httpd::Response ApiRequestHandler::onRequest(const httpd::Request& request)
{
    try
    {
        if (request._method == httpd::Method::OPTIONS)
        {
            return httpd::Response(httpd::StatusCode::NO_CONTENT);
        }

        auto token = utils::StringTokenizer::tokenize(request._url.c_str(), request._url.length(), '/');

#if ENABLE_LEGACY_API
        if (utils::StringTokenizer::isEqual(token, "colibri"))
        {
            return _legacyApiRequestHandler->onRequest(request);
        }
#endif

        if (utils::StringTokenizer::isEqual(token, "about"))
        {
            return handleAbout(request, token);
        }

        if (utils::StringTokenizer::isEqual(token, "stats"))
        {
            return handleStats(request);
        }

        RequestLogger requestLogger(request, _lastAutoRequestId);
        try
        {
            if (utils::StringTokenizer::isEqual(token, "conferences") && !token.next)
            {
                if (request._method == httpd::Method::GET)
                {
                    nlohmann::json responseBodyJson = nlohmann::json::array();
                    for (const auto& mixerId : _mixerManager.getMixerIds())
                    {
                        responseBodyJson.push_back(mixerId);
                    }

                    httpd::Response response(httpd::StatusCode::OK, responseBodyJson.dump(4));
                    response._headers["Content-type"] = "text/json";
                    requestLogger.setResponse(response);
                    return response;
                }
                else if (request._method == httpd::Method::POST)
                {
                    return allocateConference(requestLogger, request);
                }
                else
                {
                    throw httpd::RequestErrorException(httpd::StatusCode::METHOD_NOT_ALLOWED);
                }
            }
            else if (utils::StringTokenizer::isEqual(token, "conferences") && token.next)
            {
                if (request._method != httpd::Method::POST)
                {
                    throw httpd::RequestErrorException(httpd::StatusCode::METHOD_NOT_ALLOWED);
                }

                token = utils::StringTokenizer::tokenize(token, '/');
                const auto conferenceId = token.str();
                if (!token.next)
                {
                    throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
                }
                token = utils::StringTokenizer::tokenize(token, '/');
                const auto endpointId = token.str();

                const auto requestBody = request._body.build();
                const auto requestBodyJson = nlohmann::json::parse(requestBody);
                const auto actionJsonItr = requestBodyJson.find("action");
                if (actionJsonItr == requestBodyJson.end())
                {
                    throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
                }
                const auto& action = actionJsonItr->get<std::string>();

                if (action.compare("allocate") == 0)
                {
                    const auto allocateChannel = api::Parser::parseAllocateEndpoint(requestBodyJson);
                    return allocateEndpoint(requestLogger, allocateChannel, conferenceId, endpointId);
                }
                else if (action.compare("configure") == 0)
                {
                    const auto endpointDescription = api::Parser::parsePatchEndpoint(requestBodyJson, endpointId);
                    return configureEndpoint(requestLogger, endpointDescription, conferenceId, endpointId);
                }
                else if (action.compare("reconfigure") == 0)
                {
                    const auto endpointDescription = api::Parser::parsePatchEndpoint(requestBodyJson, endpointId);
                    return reconfigureEndpoint(requestLogger, endpointDescription, conferenceId, endpointId);
                }
                else if (action.compare("record") == 0)
                {
                    const auto recording = api::Parser::parseRecording(requestBodyJson);
                    return recordEndpoint(requestLogger, recording, conferenceId);
                }
                else if (action.compare("expire") == 0)
                {
                    return expireEndpoint(requestLogger, conferenceId, endpointId);
                }
                else
                {
                    throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
                }
            }
        }
        catch (httpd::RequestErrorException e)
        {
            httpd::Response response(e.getStatusCode());
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

        httpd::Response response(httpd::StatusCode::NOT_FOUND);
        requestLogger.setResponse(response);
        return response;
    }
    catch (...)
    {
        logger::error("Uncaught exception in onRequest", "ApiRequestHandler");
        return httpd::Response(httpd::StatusCode::INTERNAL_SERVER_ERROR);
    }
}

httpd::Response ApiRequestHandler::handleStats(const httpd::Request& request)
{
    if (request._method != httpd::Method::GET)
    {
        return httpd::Response(httpd::StatusCode::METHOD_NOT_ALLOWED);
    }

    auto stats = _mixerManager.getStats();
    const auto statsDescription = stats.describe();
    httpd::Response response(httpd::StatusCode::OK, statsDescription);
    response._headers["Content-type"] = "text/json";
    return response;
}

httpd::Response ApiRequestHandler::handleAbout(const httpd::Request& request,
    const utils::StringTokenizer::Token& token)
{
    if (request._method != httpd::Method::GET)
    {
        return httpd::Response(httpd::StatusCode::METHOD_NOT_ALLOWED);
    }

    const auto nextToken = utils::StringTokenizer::tokenize(token.next, token.remainingLength, '/');
    if (utils::StringTokenizer::isEqual(nextToken, "version"))
    {
        httpd::Response response(httpd::StatusCode::OK, "{}");
        response._headers["Content-type"] = "text/json";
        return response;
    }
    else if (utils::StringTokenizer::isEqual(nextToken, "health"))
    {
        httpd::Response response(httpd::StatusCode::OK, "{}");
        response._headers["Content-type"] = "text/json";
        return response;
    }
    else
    {
        return httpd::Response(httpd::StatusCode::NOT_FOUND);
    }
}

httpd::Response ApiRequestHandler::allocateConference(RequestLogger& requestLogger, const httpd::Request& request)
{
    const auto requestBody = request._body.build();
    const auto requestBodyJson = nlohmann::json::parse(requestBody);
    if (!requestBodyJson.is_object())
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
    }

    const auto allocateConference = api::Parser::parseAllocateConference(requestBodyJson);

    auto mixer = allocateConference._lastN.isSet() ? _mixerManager.create(allocateConference._lastN.get())
                                                   : _mixerManager.create();
    if (!mixer)
    {
        throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
    }

    logger::info("Allocate conference %s, mixer %s, last-n %d",
        "ApiRequestHandler",
        mixer->getId().c_str(),
        mixer->getLoggableId().c_str(),
        allocateConference._lastN.isSet() ? allocateConference._lastN.get() : -1);

    nlohmann::json responseJson;
    responseJson["id"] = mixer->getId();

    httpd::Response response(httpd::StatusCode::OK, responseJson.dump(4));
    response._headers["Content-type"] = "text/json";
    requestLogger.setResponse(response);
    return response;
}

httpd::Response ApiRequestHandler::allocateEndpoint(RequestLogger& requestLogger,
    const api::AllocateEndpoint& allocateChannel,
    const std::string& conferenceId,
    const std::string& endpointId)
{
    Mixer* mixer;
    auto scopedMixerLock = _mixerManager.getMixer(conferenceId, mixer);
    assert(scopedMixerLock.owns_lock());
    if (!mixer)
    {
        throw httpd::RequestErrorException(httpd::StatusCode::NOT_FOUND);
    }

    utils::Optional<std::string> audioChannelId;
    utils::Optional<std::string> videoChannelId;
    utils::Optional<std::string> dataChannelId;

    if (allocateChannel._bundleTransport.isSet())
    {
        const auto& bundleTransport = allocateChannel._bundleTransport.get();
        if (!bundleTransport._ice || !bundleTransport._dtls)
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
        }

        const auto iceRole = bundleTransport._iceControlling.isSet() && !bundleTransport._iceControlling.get()
            ? ice::IceRole::CONTROLLED
            : ice::IceRole::CONTROLLING;

        mixer->addBundleTransportIfNeeded(endpointId, iceRole);

        if (allocateChannel._audio.isSet())
        {
            const auto& audio = allocateChannel._audio.get();
            const auto mixed = audio._relayType.compare("mixed") == 0;
            const auto ssrcRewrite = audio._relayType.compare("ssrc-rewrite") == 0;

            std::string outChannelId;
            if (!mixer->addBundledAudioStream(outChannelId, endpointId, mixed, ssrcRewrite))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
            }
            audioChannelId.set(outChannelId);
        }

        if (allocateChannel._video.isSet())
        {
            const auto& video = allocateChannel._video.get();
            const auto ssrcRewrite = video._relayType.compare("ssrc-rewrite") == 0;

            std::string outChannelId;
            if (!mixer->addBundledVideoStream(outChannelId, endpointId, ssrcRewrite))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
            }
            videoChannelId.set(outChannelId);
        }

        if (allocateChannel._data.isSet())
        {
            std::string outChannelId;
            if (!mixer->addBundledDataStream(outChannelId, endpointId))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
            }
            dataChannelId.set(outChannelId);
        }
    }
    else
    {
        if (allocateChannel._audio.isSet())
        {
            const auto& audio = allocateChannel._audio.get();
            if (!audio._transport.isSet())
            {
                throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
            }

            const auto& transport = audio._transport.get();
            utils::Optional<ice::IceRole> iceRole;
            if (transport._ice)
            {
                iceRole.set(transport._iceControlling.isSet() && !transport._iceControlling.get()
                        ? ice::IceRole::CONTROLLED
                        : ice::IceRole::CONTROLLING);
            }
            const auto mixed = audio._relayType.compare("mixed") == 0;

            std::string outChannelId;
            if (!mixer->addAudioStream(outChannelId, endpointId, iceRole, mixed, false))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
            }
            audioChannelId.set(outChannelId);
        }

        if (allocateChannel._video.isSet())
        {
            const auto& video = allocateChannel._video.get();
            if (!video._transport.isSet())
            {
                throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
            }

            const auto& transport = video._transport.get();
            utils::Optional<ice::IceRole> iceRole;
            if (transport._ice)
            {
                iceRole.set(transport._iceControlling.isSet() && !transport._iceControlling.get()
                        ? ice::IceRole::CONTROLLED
                        : ice::IceRole::CONTROLLING);
            }

            std::string outChannelId;
            if (!mixer->addVideoStream(outChannelId, endpointId, iceRole, false))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
            }
            videoChannelId.set(outChannelId);
        }

        if (allocateChannel._data.isSet())
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
        }
    }

    uint32_t totalSleepTimeMs = 0;

    if (allocateChannel._audio.isSet())
    {
        while (!mixer->isAudioStreamGatheringComplete(endpointId) && totalSleepTimeMs < gatheringCompleteMaxWaitMs)
        {
            totalSleepTimeMs += gatheringCompleteWaitMs;
            usleep(gatheringCompleteWaitMs * 1000);
        }
    }

    if (allocateChannel._video.isSet())
    {
        while (!mixer->isVideoStreamGatheringComplete(endpointId) && totalSleepTimeMs < gatheringCompleteMaxWaitMs)
        {
            totalSleepTimeMs += gatheringCompleteWaitMs;
            usleep(gatheringCompleteWaitMs * 1000);
        }
    }

    if (allocateChannel._data.isSet())
    {
        while (!mixer->isDataStreamGatheringComplete(endpointId) && totalSleepTimeMs < gatheringCompleteMaxWaitMs)
        {
            totalSleepTimeMs += gatheringCompleteWaitMs;
            usleep(gatheringCompleteWaitMs * 1000);
        }
    }

    if (totalSleepTimeMs >= gatheringCompleteWaitMs)
    {
        logger::error("Allocate endpoint id %s, mixer %s, gathering did not complete in time",
            "RequestHandler",
            endpointId.c_str(),
            conferenceId.c_str());

        if (audioChannelId.isSet())
        {
            mixer->removeAudioStream(audioChannelId.get());
        }

        if (videoChannelId.isSet())
        {
            mixer->removeVideoStream(videoChannelId.get());
        }

        if (dataChannelId.isSet())
        {
            mixer->removeDataStream(dataChannelId.get());
        }

        throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
    }

    return generateAllocateEndpointResponse(requestLogger, allocateChannel, *mixer, conferenceId, endpointId);
}

httpd::Response ApiRequestHandler::generateAllocateEndpointResponse(RequestLogger& requestLogger,
    const api::AllocateEndpoint& allocateChannel,
    Mixer& mixer,
    const std::string& conferenceId,
    const std::string& endpointId)
{
    api::EndpointDescription channelsDescription;
    channelsDescription._endpointId = endpointId;

    // Describe bundle transport
    if (allocateChannel._bundleTransport.isSet())
    {
        const auto& bundleTransport = allocateChannel._bundleTransport.get();
        api::EndpointDescription::Transport responseBundleTransport;

        TransportDescription transportDescription;
        if (!mixer.getTransportBundleDescription(endpointId, transportDescription))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
        }

        if (!bundleTransport._ice || !bundleTransport._dtls)
        {
            logger::warn("Bundling without ice not supported, conference %s",
                "ApiRequestHandler",
                conferenceId.c_str());
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
        }

        const auto& transportDescriptionIce = transportDescription._ice.get();
        api::EndpointDescription::Ice responseIce;
        responseIce._ufrag = transportDescriptionIce._iceCredentials.first;
        responseIce._pwd = transportDescriptionIce._iceCredentials.second;
        for (const auto& iceCandidate : transportDescriptionIce._iceCandidates)
        {
            if (iceCandidate.type != ice::IceCandidate::Type::PRFLX)
            {
                responseIce._candidates.emplace_back(iceCandidateToApi(iceCandidate));
            }
        }
        responseBundleTransport._ice.set(responseIce);

        api::EndpointDescription::Dtls responseDtls;
        responseDtls._type = "sha-256";
        responseDtls._hash = _sslDtls.getLocalFingerprint();
        responseDtls._setup = "actpass";
        responseBundleTransport._dtls.set(responseDtls);

        responseBundleTransport._rtcpMux = true;
        channelsDescription._bundleTransport.set(responseBundleTransport);
    }

    // Describe audio, video and data streams
    if (allocateChannel._audio.isSet())
    {
        const auto& audio = allocateChannel._audio.get();
        api::EndpointDescription::Audio responseAudio;

        StreamDescription streamDescription;
        TransportDescription transportDescription;

        if (!mixer.getAudioStreamDescription(endpointId, streamDescription) ||
            !mixer.getAudioStreamTransportDescription(endpointId, transportDescription))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
        }

        responseAudio._ssrcs = streamDescription._localSsrcs;

        if (audio._transport.isSet())
        {
            const auto& transport = audio._transport.get();
            api::EndpointDescription::Transport responseTransport;

            if (transport._ice)
            {
                api::EndpointDescription::Ice responseIce;
                const auto& transportDescriptionIce = transportDescription._ice.get();
                responseIce._ufrag = transportDescriptionIce._iceCredentials.first;
                responseIce._pwd = transportDescriptionIce._iceCredentials.second;
                for (const auto& iceCandidate : transportDescriptionIce._iceCandidates)
                {
                    if (iceCandidate.type != ice::IceCandidate::Type::PRFLX)
                    {
                        responseIce._candidates.emplace_back(iceCandidateToApi(iceCandidate));
                    }
                }
                responseTransport._ice.set(responseIce);
                responseTransport._rtcpMux = true;
            }
            else
            {
                if (transportDescription._localPeer.isSet() && !transportDescription._localPeer.get().empty())
                {
                    api::EndpointDescription::Connection responseConnection;
                    responseConnection._ip = transportDescription._localPeer.get().ipToString();
                    responseConnection._port = transportDescription._localPeer.get().getPort();
                    responseTransport._connection.set(responseConnection);
                }

                responseTransport._rtcpMux = false;
            }

            if (transportDescription._dtls.isSet())
            {
                api::EndpointDescription::Dtls responseDtls;
                responseDtls._setup = "active";
                responseDtls._type = "sha-256";
                responseDtls._hash = _sslDtls.getLocalFingerprint();
                responseTransport._dtls.set(responseDtls);
            }

            responseAudio._transport.set(responseTransport);
        }

        addDefaultAudioProperties(responseAudio);
        channelsDescription._audio.set(responseAudio);
    }

    if (allocateChannel._video.isSet())
    {
        const auto& video = allocateChannel._video.get();
        api::EndpointDescription::Video responseVideo;

        StreamDescription streamDescription;
        TransportDescription transportDescription;

        if (!mixer.getVideoStreamDescription(endpointId, streamDescription) ||
            !mixer.getVideoStreamTransportDescription(endpointId, transportDescription))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
        }

        responseVideo._ssrcs = streamDescription._localSsrcs;
        if (responseVideo._ssrcs.size() > 1)
        {
            for (size_t i = 1; i < responseVideo._ssrcs.size() - 1; i += 2)
            {
                api::EndpointDescription::SsrcGroup responseSsrcGroup;
                responseSsrcGroup._ssrcs.push_back(responseVideo._ssrcs[i]);
                responseSsrcGroup._ssrcs.push_back(responseVideo._ssrcs[i + 1]);
                responseSsrcGroup._semantics = "FID";
                responseVideo._ssrcGroups.push_back(responseSsrcGroup);
            }

            api::EndpointDescription::SsrcAttribute responseSsrcAttribute;
            responseSsrcAttribute._ssrcs.push_back(responseVideo._ssrcs[1]);
            responseSsrcAttribute._content = "slides";
            responseVideo._ssrcAttributes.push_back(responseSsrcAttribute);
        }

        if (video._transport.isSet())
        {
            const auto& transport = video._transport.get();
            api::EndpointDescription::Transport responseTransport;

            if (transport._ice)
            {
                api::EndpointDescription::Ice responseIce;
                const auto& transportDescriptionIce = transportDescription._ice.get();
                responseIce._ufrag = transportDescriptionIce._iceCredentials.first;
                responseIce._pwd = transportDescriptionIce._iceCredentials.second;
                for (const auto& iceCandidate : transportDescriptionIce._iceCandidates)
                {
                    if (iceCandidate.type != ice::IceCandidate::Type::PRFLX)
                    {
                        responseIce._candidates.emplace_back(iceCandidateToApi(iceCandidate));
                    }
                }
                responseTransport._ice.set(responseIce);
                responseTransport._rtcpMux = true;
            }
            else
            {
                if (transportDescription._localPeer.isSet() && !transportDescription._localPeer.get().empty())
                {
                    api::EndpointDescription::Connection responseConnection;
                    responseConnection._ip = transportDescription._localPeer.get().ipToString();
                    responseConnection._port = transportDescription._localPeer.get().getPort();
                    responseTransport._connection.set(responseConnection);
                }

                responseTransport._rtcpMux = false;
            }

            if (transportDescription._dtls.isSet())
            {
                api::EndpointDescription::Dtls responseDtls;
                responseDtls._setup = "active";
                responseDtls._type = "sha-256";
                responseDtls._hash = _sslDtls.getLocalFingerprint();
                responseTransport._dtls.set(responseDtls);
            }

            responseVideo._transport.set(responseTransport);
        }

        addDefaultVideoProperties(responseVideo);
        channelsDescription._video.set(responseVideo);
    }

    if (allocateChannel._data.isSet())
    {
        api::EndpointDescription::Data responseData;

        DataStreamDescription streamDescription;
        if (!mixer.getDataStreamDescription(endpointId, streamDescription))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
        }

        responseData._port = streamDescription._sctpPort.isSet() ? streamDescription._sctpPort.get() : 5000;
        channelsDescription._data.set(responseData);
    }

    const auto responseBody = api::Generator::generateAllocateEndpointResponse(channelsDescription);
    auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
    response._headers["Content-type"] = "text/json";
    logger::debug("PATCH response %s", "RequestHandler", response._body.c_str());
    requestLogger.setResponse(response);
    return response;
}

httpd::Response ApiRequestHandler::configureEndpoint(RequestLogger& requestLogger,
    const api::EndpointDescription& endpointDescription,
    const std::string& conferenceId,
    const std::string& endpointId)
{
    Mixer* mixer;
    auto scopedMixerLock = _mixerManager.getMixer(conferenceId, mixer);
    assert(scopedMixerLock.owns_lock());
    if (!mixer)
    {
        throw httpd::RequestErrorException(httpd::StatusCode::NOT_FOUND);
    }

    if (endpointDescription._audio.isSet())
    {
        configureAudioEndpoint(endpointDescription, *mixer, endpointId);
    }

    if (endpointDescription._video.isSet())
    {
        configureVideoEndpoint(endpointDescription, *mixer, endpointId);
    }

    if (endpointDescription._data.isSet())
    {
        configureDataEndpoint(endpointDescription, *mixer, endpointId);
    }

    if (endpointDescription._bundleTransport.isSet())
    {
        const auto& transport = endpointDescription._bundleTransport.get();

        if (transport._ice.isSet())
        {
            const auto candidatesAndCredentials = getIceCandidatesAndCredentials(transport);
            if (!mixer->configureBundleTransportIce(endpointId,
                    candidatesAndCredentials.second,
                    candidatesAndCredentials.first))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
            }
        }

        if (transport._dtls.isSet())
        {
            const auto& dtls = transport._dtls.get();
            const bool isRemoteSideDtlsClient = dtls._setup.compare("active") == 0;

            if (!mixer->configureBundleTransportDtls(endpointId, dtls._type, dtls._hash, !isRemoteSideDtlsClient))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
            }
        }

        if (!mixer->startBundleTransport(endpointId))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
        }
    }

    const auto responseBody = nlohmann::json::object();
    auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
    response._headers["Content-type"] = "text/json";
    requestLogger.setResponse(response);
    return response;
}

void ApiRequestHandler::configureAudioEndpoint(const api::EndpointDescription& endpointDescription,
    Mixer& mixer,
    const std::string& endpointId)
{
    const auto& audio = endpointDescription._audio.get();
    if (mixer.isAudioStreamConfigured(endpointId) || !audio._payloadType.isSet())
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
    }

    const auto rtpMap = makeRtpMap(audio._payloadType.get());
    const auto absSendTimeExtensionId = findAbsSendTimeExtensionId(audio._rtpHeaderExtensions);

    utils::Optional<uint32_t> remoteSsrc;
    if (!audio._ssrcs.empty())
    {
        remoteSsrc.set(audio._ssrcs.front());
    }

    utils::Optional<int32_t> audioLevelExtensionId;
    for (const auto& rtpHeaderExtension : audio._rtpHeaderExtensions)
    {
        if (rtpHeaderExtension.second.compare("urn:ietf:params:rtp-hdrext:ssrc-audio-level") == 0)
        {
            audioLevelExtensionId.set(utils::checkedCast<int32_t>(rtpHeaderExtension.first));
        }
    }

    if (!mixer.configureAudioStream(endpointId, rtpMap, remoteSsrc, audioLevelExtensionId, absSendTimeExtensionId))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
    }

    if (audio._transport.isSet())
    {
        const auto& transport = audio._transport.get();
        if (transport._ice.isSet())
        {
            const auto& candidatesAndCredentials = getIceCandidatesAndCredentials(transport);
            if (!mixer.configureAudioStreamTransportIce(endpointId,
                    candidatesAndCredentials.second,
                    candidatesAndCredentials.first))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
            }
        }
        else if (transport._connection.isSet())
        {
            const auto& connection = transport._connection.get();
            const auto remotePeer = transport::SocketAddress::parse(connection._ip, connection._port);
            if (!mixer.configureAudioStreamTransportConnection(endpointId, remotePeer))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
            }
        }

        if (!transport._dtls.isSet())
        {
            const auto& dtls = transport._dtls.get();
            const bool isRemoteSideDtlsClient = dtls._setup.compare("active") == 0;

            if (!mixer.configureAudioStreamTransportDtls(endpointId, dtls._type, dtls._hash, !isRemoteSideDtlsClient))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
            }
        }
        else
        {
            if (!mixer.configureAudioStreamTransportDisableDtls(endpointId))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
            }
        }

        if (!mixer.addAudioStreamToEngine(endpointId) || !mixer.startAudioStreamTransport(endpointId))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
        }
    }
    else
    {
        if (!mixer.addAudioStreamToEngine(endpointId))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
        }
    }
}

void ApiRequestHandler::configureVideoEndpoint(const api::EndpointDescription& endpointDescription,
    Mixer& mixer,
    const std::string& endpointId)
{
    const auto& video = endpointDescription._video.get();
    if (mixer.isVideoStreamConfigured(endpointId) || video._payloadTypes.empty())
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
    }

    std::vector<RtpMap> rtpMaps;
    for (const auto& payloadType : video._payloadTypes)
    {
        rtpMaps.emplace_back(makeRtpMap(payloadType));
    }
    const auto absSendTimeExtensionId = findAbsSendTimeExtensionId(video._rtpHeaderExtensions);

    const auto feedbackRtpMap = rtpMaps.size() > 1 ? rtpMaps[1] : RtpMap();
    auto simulcastStreams = makeSimulcastStreams(video, endpointId);
    if (simulcastStreams.size() > 2)
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
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

    const auto ssrcWhitelist = makeWhitelistedSsrcsArray(video);

    if (!mixer.configureVideoStream(endpointId,
            rtpMaps.front(),
            feedbackRtpMap,
            simulcastStreams[0],
            secondarySimulcastStream,
            absSendTimeExtensionId,
            ssrcWhitelist))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
    }

    if (video._transport.isSet())
    {
        const auto& transport = video._transport.get();

        if (transport._ice.isSet())
        {
            const auto& candidatesAndCredentials = getIceCandidatesAndCredentials(transport);
            if (!mixer.configureVideoStreamTransportIce(endpointId,
                    candidatesAndCredentials.second,
                    candidatesAndCredentials.first))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
            }
        }
        else if (transport._connection.isSet())
        {
            const auto& connection = transport._connection.get();
            const auto remotePeer = transport::SocketAddress::parse(connection._ip, connection._port);
            if (!mixer.configureVideoStreamTransportConnection(endpointId, remotePeer))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
            }
        }

        if (!transport._dtls.isSet())
        {
            const auto& dtls = transport._dtls.get();
            const bool isRemoteSideDtlsClient = dtls._setup.compare("active") == 0;

            if (!mixer.configureVideoStreamTransportDtls(endpointId, dtls._type, dtls._hash, !isRemoteSideDtlsClient))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
            }
        }
        else
        {
            if (!mixer.configureVideoStreamTransportDisableDtls(endpointId))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
            }
        }

        if (!mixer.addVideoStreamToEngine(endpointId) || !mixer.startVideoStreamTransport(endpointId))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
        }
    }
    else
    {
        if (!mixer.addVideoStreamToEngine(endpointId))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
        }
    }
}

void ApiRequestHandler::configureDataEndpoint(const api::EndpointDescription& endpointDescription,
    Mixer& mixer,
    const std::string& endpointId)
{
    const auto& data = endpointDescription._data.get();
    if (!mixer.configureDataStream(endpointId, data._port) || !mixer.addDataStreamToEngine(endpointId))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
    }
}

httpd::Response ApiRequestHandler::reconfigureEndpoint(RequestLogger& requestLogger,
    const api::EndpointDescription& endpointDescription,
    const std::string& conferenceId,
    const std::string& endpointId)
{
    Mixer* mixer;
    auto scopedMixerLock = _mixerManager.getMixer(conferenceId, mixer);
    assert(scopedMixerLock.owns_lock());
    if (!mixer)
    {
        throw httpd::RequestErrorException(httpd::StatusCode::NOT_FOUND);
    }

    if (!mixer->isAudioStreamConfigured(endpointId) || !mixer->isVideoStreamConfigured(endpointId))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
    }

    if (endpointDescription._audio.isSet())
    {
        const auto& audio = endpointDescription._audio.get();
        utils::Optional<uint32_t> remoteSsrc;
        if (!audio._ssrcs.empty())
        {
            remoteSsrc.set(audio._ssrcs.front());
        }

        if (!mixer->reconfigureAudioStream(endpointId, remoteSsrc))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
        }
    }

    if (endpointDescription._video.isSet())
    {
        const auto& video = endpointDescription._video.get();
        auto simulcastStreams = makeSimulcastStreams(video, endpointId);
        if (simulcastStreams.size() > 2)
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
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

        const auto ssrcWhitelist = makeWhitelistedSsrcsArray(video);
        if (!mixer->reconfigureVideoStream(endpointId, simulcastStreams[0], secondarySimulcastStream, ssrcWhitelist))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST);
        }
    }

    const auto responseBody = nlohmann::json::object();
    auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
    response._headers["Content-type"] = "text/json";
    requestLogger.setResponse(response);
    return response;
}

httpd::Response ApiRequestHandler::recordEndpoint(RequestLogger& requestLogger,
    const api::Recording& recording,
    const std::string& conferenceId)
{
    Mixer* mixer;
    auto scopedMixerLock = _mixerManager.getMixer(conferenceId, mixer);
    assert(scopedMixerLock.owns_lock());
    if (!mixer)
    {
        throw httpd::RequestErrorException(httpd::StatusCode::NOT_FOUND);
    }

    const bool isRecordingStart =
        recording._isAudioEnabled || recording._isVideoEnabled || recording._isScreenshareEnabled;

    if (isRecordingStart)
    {
        RecordingDescription description;
        description._ownerId = recording._userId;
        description._recordingId = recording._recordingId;
        description._isAudioEnabled = recording._isAudioEnabled;
        description._isVideoEnabled = recording._isVideoEnabled;
        description._isScreenSharingEnabled = recording._isScreenshareEnabled;

        if (!mixer->addOrUpdateRecording(conferenceId, recording._channels, description))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
        }
    }
    else if (!recording._channels.empty())
    {
        if (!mixer->removeRecordingTransports(conferenceId, recording._channels))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
        }
    }
    else
    {
        if (!mixer->removeRecording(recording._recordingId))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR);
        }
    }

    const auto responseBody = nlohmann::json::object();
    auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
    response._headers["Content-type"] = "text/json";
    requestLogger.setResponse(response);
    return response;
}

httpd::Response ApiRequestHandler::expireEndpoint(RequestLogger& requestLogger,
    const std::string& conferenceId,
    const std::string& endpointId)
{
    Mixer* mixer;
    auto scopedMixerLock = _mixerManager.getMixer(conferenceId, mixer);
    assert(scopedMixerLock.owns_lock());
    if (!mixer)
    {
        throw httpd::RequestErrorException(httpd::StatusCode::NOT_FOUND);
    }

    mixer->removeAudioStream(endpointId);
    mixer->removeVideoStream(endpointId);
    mixer->removeDataStream(endpointId);

    const auto responseBody = nlohmann::json::object();
    auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
    response._headers["Content-type"] = "text/json";
    requestLogger.setResponse(response);
    return response;
}

} // namespace bridge
