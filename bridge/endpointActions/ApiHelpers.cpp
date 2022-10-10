#include "ApiHelpers.h"
#include "ActionContext.h"
#include "api/EndpointDescription.h"
#include "bridge/Mixer.h"
#include "bridge/MixerManager.h"
#include "codec/Opus.h"
#include "codec/Vp8.h"
#include "httpd/RequestErrorException.h"
#include "utils/CheckedCast.h"
#include "utils/Format.h"

namespace bridge
{
std::unique_lock<std::mutex> getConferenceMixer(ActionContext* context,
    const std::string& conferenceId,
    Mixer*& outMixer)
{
    auto scopedMixerLock = context->mixerManager.getMixer(conferenceId, outMixer);
    assert(scopedMixerLock.owns_lock());
    if (!outMixer)
    {
        throw httpd::RequestErrorException(httpd::StatusCode::NOT_FOUND,
            utils::format("Conference '%s' not found", conferenceId.c_str()));
    }
    return scopedMixerLock;
}

void addDefaultAudioProperties(api::Audio& audioChannel)
{
    api::PayloadType opus;
    opus.id = codec::Opus::payloadType;
    opus.name = "opus";
    opus.clockRate = codec::Opus::sampleRate;
    opus.channels.set(codec::Opus::channelsPerFrame);
    opus.parameters.emplace_back("minptime", "10");
    opus.parameters.emplace_back("useinbandfec", "1");

    audioChannel.payloadType.set(opus);
    audioChannel.rtpHeaderExtensions.emplace_back(1, "urn:ietf:params:rtp-hdrext:ssrc-audio-level");
    audioChannel.rtpHeaderExtensions.emplace_back(3, "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time");
    audioChannel.rtpHeaderExtensions.emplace_back(8, "c9:params:rtp-hdrext:info");
}

void addDefaultVideoProperties(api::Video& videoChannel)
{
    {
        api::PayloadType vp8;
        vp8.id = codec::Vp8::payloadType;
        vp8.name = "VP8";
        vp8.clockRate = codec::Vp8::sampleRate;
        vp8.rtcpFeedbacks.emplace_back("goog-remb", utils::Optional<std::string>());
        vp8.rtcpFeedbacks.emplace_back("nack", utils::Optional<std::string>());
        vp8.rtcpFeedbacks.emplace_back("nack", utils::Optional<std::string>("pli"));
        videoChannel.payloadTypes.push_back(vp8);
    }

    {
        api::PayloadType vp8Rtx;
        vp8Rtx.id = codec::Vp8::rtxPayloadType;
        vp8Rtx.name = "rtx";
        vp8Rtx.clockRate = codec::Vp8::sampleRate;
        vp8Rtx.parameters.emplace_back("apt", std::to_string(codec::Vp8::payloadType));
        videoChannel.payloadTypes.push_back(vp8Rtx);
    }

    videoChannel.rtpHeaderExtensions.emplace_back(3, "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time");
    videoChannel.rtpHeaderExtensions.emplace_back(4, "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id");
}

ice::TransportType parseTransportType(const std::string& protocol)
{
    if (protocol.compare("udp") == 0)
    {
        return ice::TransportType::UDP;
    }

    if (protocol.compare("tcp") == 0)
    {
        return ice::TransportType::TCP;
    }

    if (protocol.compare("ssltcp") == 0)
    {
        return ice::TransportType::SSLTCP;
    }

    throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
        utils::format("Transport protocol type '%s' not supported", protocol.c_str()));
}

api::Candidate iceCandidateToApi(const ice::IceCandidate& iceCandidate)
{
    api::Candidate candidate;
    candidate.generation = 0;
    candidate.component = iceCandidate.component == ice::IceComponent::RTP ? 1 : 2;

    switch (iceCandidate.transportType)
    {
    case ice::TransportType::UDP:
        candidate.protocol = "udp";
        break;
    case ice::TransportType::TCP:
        candidate.protocol = "tcp";
        break;
    case ice::TransportType::SSLTCP:
        candidate.protocol = "ssltcp";
        break;
    default:
        assert(false);
        break;
    }

    candidate.port = iceCandidate.address.getPort();
    candidate.ip = iceCandidate.address.ipToString();

    switch (iceCandidate.type)
    {
    case ice::IceCandidate::Type::HOST:
        candidate.type = "host";
        break;
    case ice::IceCandidate::Type::SRFLX:
        candidate.type = "srflx";
        candidate.relPort.set(iceCandidate.baseAddress.getPort());
        candidate.relAddr.set(iceCandidate.baseAddress.ipToString());
        break;
    case ice::IceCandidate::Type::PRFLX:
        candidate.type = "prflx";
        candidate.relPort.set(iceCandidate.baseAddress.getPort());
        candidate.relAddr.set(iceCandidate.baseAddress.ipToString());
        break;
    default:
        candidate.type = "unsupported";
        assert(false);
        break;
    }

    candidate.foundation = iceCandidate.getFoundation();
    candidate.priority = iceCandidate.priority;
    candidate.network = 1;

    return candidate;
}

std::pair<std::vector<ice::IceCandidate>, std::pair<std::string, std::string>> getIceCandidatesAndCredentials(
    const api::Transport& transport)
{
    const auto& ice = transport.ice.get();
    return getIceCandidatesAndCredentials(ice);
}

std::pair<std::vector<ice::IceCandidate>, std::pair<std::string, std::string>> getIceCandidatesAndCredentials(
    const api::Ice& ice)
{
    std::vector<ice::IceCandidate> candidates;

    for (const auto& candidate : ice.candidates)
    {
        const ice::TransportType transportType = parseTransportType(candidate.protocol);
        if (candidate.type.compare("host") == 0)
        {
            candidates.emplace_back(candidate.foundation.c_str(),
                candidate.component == 1 ? ice::IceComponent::RTP : ice::IceComponent::RTCP,
                transportType,
                candidate.priority,
                transport::SocketAddress::parse(candidate.ip, candidate.port),
                ice::IceCandidate::Type::HOST);
        }
        else if ((candidate.type.compare("srflx") == 0 || candidate.type.compare("relay") == 0) &&
            candidate.relAddr.isSet() && candidate.relPort.isSet())
        {
            candidates.emplace_back(candidate.foundation.c_str(),
                candidate.component == 1 ? ice::IceComponent::RTP : ice::IceComponent::RTCP,
                transportType,
                candidate.priority,
                transport::SocketAddress::parse(candidate.ip, candidate.port),
                transport::SocketAddress::parse(candidate.relAddr.get(), candidate.relPort.get()),
                candidate.type.compare("srflx") == 0 ? ice::IceCandidate::Type::SRFLX : ice::IceCandidate::Type::RELAY);
        }
    }

    return std::make_pair(candidates, std::make_pair(ice.ufrag, ice.pwd));
}

bridge::RtpMap makeRtpMap(const api::Audio& audio)
{
    bridge::RtpMap rtpMap;

    auto& payloadType = audio.payloadType.get();
    if (payloadType.name.compare("opus") == 0)
    {
        rtpMap =
            bridge::RtpMap(bridge::RtpMap::Format::OPUS, payloadType.id, payloadType.clockRate, payloadType.channels);
    }
    else
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            utils::format("rtp payload '%s' not supported", payloadType.name.c_str()));
    }

    for (const auto& parameter : payloadType.parameters)
    {
        rtpMap.parameters.emplace(parameter.first, parameter.second);
    }

    rtpMap.audioLevelExtId = findAudioLevelExtensionId(audio.rtpHeaderExtensions);
    rtpMap.absSendTimeExtId = findAbsSendTimeExtensionId(audio.rtpHeaderExtensions);
    rtpMap.c9infoExtId = findC9InfoExtensionId(audio.rtpHeaderExtensions);

    return rtpMap;
}

bridge::RtpMap makeRtpMap(const api::Video& video, const api::PayloadType& payloadType)
{
    bridge::RtpMap rtpMap;

    if (payloadType.name.compare("VP8") == 0)
    {
        rtpMap = bridge::RtpMap(bridge::RtpMap::Format::VP8, payloadType.id, payloadType.clockRate);
    }
    else if (payloadType.name.compare("rtx") == 0)
    {
        rtpMap = bridge::RtpMap(bridge::RtpMap::Format::VP8RTX, codec::Vp8::rtxPayloadType, codec::Vp8::sampleRate);
    }
    else
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            utils::format("rtp payload '%s' not supported", payloadType.name.c_str()));
    }

    for (const auto& parameter : payloadType.parameters)
    {
        rtpMap.parameters.emplace(parameter.first, parameter.second);
    }

    rtpMap.absSendTimeExtId = findAbsSendTimeExtensionId(video.rtpHeaderExtensions);

    return rtpMap;
}

utils::Optional<uint8_t> findExtensionId(const std::string& extName,
    const std::vector<std::pair<uint32_t, std::string>>& rtpHeaderExtensions)
{
    for (const auto& rtpHeaderExtension : rtpHeaderExtensions)
    {
        if (rtpHeaderExtension.second.compare(extName) == 0)
        {
            return utils::Optional<uint8_t>((::utils::checkedCast<uint8_t>(rtpHeaderExtension.first)));
        }
    }
    return utils::Optional<uint8_t>();
}

utils::Optional<uint8_t> findAudioLevelExtensionId(
    const std::vector<std::pair<uint32_t, std::string>>& rtpHeaderExtensions)
{
    return findExtensionId("urn:ietf:params:rtp-hdrext:ssrc-audio-level", rtpHeaderExtensions);
}

utils::Optional<uint8_t> findAbsSendTimeExtensionId(
    const std::vector<std::pair<uint32_t, std::string>>& rtpHeaderExtensions)
{
    return findExtensionId("http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time", rtpHeaderExtensions);
}

utils::Optional<uint8_t> findC9InfoExtensionId(const std::vector<std::pair<uint32_t, std::string>>& rtpHeaderExtensions)
{
    return findExtensionId("c9:params:rtp-hdrext:info", rtpHeaderExtensions);
}

std::vector<bridge::SimulcastStream> makeSimulcastStreams(const api::Video& video, const std::string& endpointId)
{
    std::vector<bridge::SimulcastStream> simulcastStreams;
    for (const auto& stream : video.streams)
    {
        bridge::SimulcastStream simulcastStream{0};
        if (stream.content.compare(api::VideoStream::slidesContent) == 0)
        {
            simulcastStream.contentType = bridge::SimulcastStream::VideoContentType::SLIDES;
        }

        for (auto& level : stream.sources)
        {
            simulcastStream.levels[simulcastStream.numLevels++] = SimulcastLevel{level.main, level.feedback, false};
            logger::debug("Add simulcast level main ssrc %u feedback ssrc %u, content %s, endpointId %s",
                "ApiRequestHandler",
                level.main,
                level.feedback,
                stream.content.c_str(),
                endpointId.c_str());
        }

        simulcastStreams.push_back(simulcastStream);
    }

    if (simulcastStreams.size() > 2)
    {
        return std::vector<bridge::SimulcastStream>(simulcastStreams.end() - 2, simulcastStreams.end());
    }

    return simulcastStreams;
}

bridge::SsrcWhitelist makeWhitelistedSsrcsArray(const api::Video& video)
{
    bridge::SsrcWhitelist ssrcWhitelist = {false, 0, {0, 0}};

    if (video.ssrcWhitelist.isSet())
    {
        ssrcWhitelist.enabled = true;
        ssrcWhitelist.numSsrcs = std::min(video.ssrcWhitelist.get().size(), ssrcWhitelist.ssrcs.size());
        for (size_t i = 0; i < ssrcWhitelist.numSsrcs; ++i)
        {
            ssrcWhitelist.ssrcs[i] = video.ssrcWhitelist.get()[i];
        }
    }

    return ssrcWhitelist;
}

} // namespace bridge
