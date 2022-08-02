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
    audioChannel._rtpHeaderExtensions.emplace_back(8, "c9:params:rtp-hdrext:info");
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

std::pair<std::vector<ice::IceCandidate>, std::pair<std::string, std::string>> getIceCandidatesAndCredentials(
    const api::EndpointDescription::Transport& transport)
{
    const auto& ice = transport._ice.get();
    std::vector<ice::IceCandidate> candidates;

    for (const auto& candidate : ice._candidates)
    {
        const ice::TransportType transportType = parseTransportType(candidate._protocol);
        if (candidate._type.compare("host") == 0)
        {
            candidates.emplace_back(candidate._foundation.c_str(),
                candidate._component == 1 ? ice::IceComponent::RTP : ice::IceComponent::RTCP,
                transportType,
                candidate._priority,
                transport::SocketAddress::parse(candidate._ip, candidate._port),
                ice::IceCandidate::Type::HOST);
        }
        else if ((candidate._type.compare("srflx") == 0 || candidate._type.compare("relay") == 0) &&
            candidate._relAddr.isSet() && candidate._relPort.isSet())
        {
            candidates.emplace_back(candidate._foundation.c_str(),
                candidate._component == 1 ? ice::IceComponent::RTP : ice::IceComponent::RTCP,
                transportType,
                candidate._priority,
                transport::SocketAddress::parse(candidate._ip, candidate._port),
                transport::SocketAddress::parse(candidate._relAddr.get(), candidate._relPort.get()),
                candidate._type.compare("srflx") == 0 ? ice::IceCandidate::Type::SRFLX
                                                      : ice::IceCandidate::Type::RELAY);
        }
    }

    return std::make_pair(candidates, std::make_pair(ice._ufrag, ice._pwd));
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
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            utils::format("rtp payload '%s' not supported", payloadType._name.c_str()));
    }

    for (const auto& parameter : payloadType._parameters)
    {
        rtpMap._parameters.emplace(parameter.first, parameter.second);
    }

    return rtpMap;
}

bridge::RtpMap makeRtpMap(const api::EndpointDescription::Audio& audio)
{
    bridge::RtpMap rtpMap;

    auto& payloadType = audio._payloadType.get();
    if (payloadType._name.compare("opus") == 0)
    {
        rtpMap = bridge::RtpMap(bridge::RtpMap::Format::OPUS,
            payloadType._id,
            payloadType._clockRate,
            payloadType._channels);
    }
    else
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            utils::format("rtp payload '%s' not supported", payloadType._name.c_str()));
    }

    for (const auto& parameter : payloadType._parameters)
    {
        rtpMap._parameters.emplace(parameter.first, parameter.second);
    }

    rtpMap._audioLevelExtId = findAudioLevelExtensionId(audio._rtpHeaderExtensions);
    rtpMap._absSendTimeExtId = findAbsSendTimeExtensionId(audio._rtpHeaderExtensions);
    rtpMap._c9infoExtId = findC9InfoExtensionId(audio._rtpHeaderExtensions);

    return rtpMap;
}

bridge::RtpMap makeRtpMap(const api::EndpointDescription::Video& video,
    const api::EndpointDescription::PayloadType& payloadType)
{
    bridge::RtpMap rtpMap;

    if (payloadType._name.compare("VP8") == 0)
    {
        rtpMap = bridge::RtpMap(bridge::RtpMap::Format::VP8, payloadType._id, payloadType._clockRate);
    }
    else if (payloadType._name.compare("rtx") == 0)
    {
        rtpMap = bridge::RtpMap(bridge::RtpMap::Format::VP8RTX, codec::Vp8::rtxPayloadType, codec::Vp8::sampleRate);
    }
    else
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            utils::format("rtp payload '%s' not supported", payloadType._name.c_str()));
    }

    for (const auto& parameter : payloadType._parameters)
    {
        rtpMap._parameters.emplace(parameter.first, parameter.second);
    }

    rtpMap._absSendTimeExtId = findAbsSendTimeExtensionId(video._rtpHeaderExtensions);

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

bridge::SsrcWhitelist makeWhitelistedSsrcsArray(const api::EndpointDescription::Video& video)
{
    bridge::SsrcWhitelist ssrcWhitelist = {false, 0, {0, 0}};

    if (video._ssrcWhitelist.isSet())
    {
        ssrcWhitelist.enabled = true;
        ssrcWhitelist.numSsrcs = std::min(video._ssrcWhitelist.get().size(), ssrcWhitelist.ssrcs.size());
        for (size_t i = 0; i < ssrcWhitelist.numSsrcs; ++i)
        {
            ssrcWhitelist.ssrcs[i] = video._ssrcWhitelist.get()[i];
        }
    }

    return ssrcWhitelist;
}

} // namespace bridge
