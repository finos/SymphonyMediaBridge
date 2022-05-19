#include "bridge/LegacyApiRequestHandlerHelpers.h"
#include "bridge/RtpMap.h"
#include "bridge/engine/SimulcastStream.h"
#include "legacyapi/Channel.h"
#include "legacyapi/SsrcGroup.h"
#include "logger/Logger.h"
#include "utils/CheckedCast.h"
#include <algorithm>

namespace
{

const legacyapi::SsrcGroup* findSimulcastGroup(const legacyapi::Channel& channel, const uint32_t ssrc)
{
    for (auto& ssrcGroup : channel._ssrcGroups)
    {
        if (ssrcGroup._semantics.compare("SIM") == 0 && ssrcGroup._sources.size() > 1)
        {
            const auto source = std::find(ssrcGroup._sources.begin(), ssrcGroup._sources.end(), ssrc);
            if (source != ssrcGroup._sources.end())
            {
                return &ssrcGroup;
            }
        }
    }
    return nullptr;
}

const legacyapi::SsrcGroup* findFeedbackGroup(const legacyapi::Channel& channel, const uint32_t ssrc)
{
    for (auto& ssrcGroup : channel._ssrcGroups)
    {
        if (ssrcGroup._semantics.compare("FID") == 0 && ssrcGroup._sources.size() == 2 && ssrcGroup._sources[0] == ssrc)
        {
            return &ssrcGroup;
        }
    }
    return nullptr;
}

} // namespace

namespace bridge
{

namespace LegacyApiRequestHandlerHelpers
{

std::vector<RtpMap> makeRtpMaps(const legacyapi::Channel& channel)
{
    std::vector<RtpMap> rtpMaps;
    if (channel._payloadTypes.empty())
    {
        return rtpMaps;
    }

    for (const auto& payloadType : channel._payloadTypes)
    {
        RtpMap rtpMap;
        if (payloadType._name.compare("opus") == 0)
        {
            rtpMaps.emplace_back(RtpMap::Format::OPUS);
        }
        else if (payloadType._name.compare("pcma") == 0)
        {
            rtpMaps.emplace_back(RtpMap::pcma());
        }
        else if (payloadType._name.compare("pcmu") == 0)
        {
            rtpMaps.emplace_back(RtpMap::pcmu());
        }
        else if (payloadType._name.compare("VP8") == 0)
        {
            rtpMaps.emplace_back(RtpMap::Format::VP8);
        }
        else if (payloadType._name.compare("H264") == 0)
        {
            rtpMaps.emplace_back(RtpMap::Format::H264);
        }
        else if (payloadType._name.compare("rtx") == 0)
        {
            rtpMaps.emplace_back(RtpMap::Format::RTX);
        }

        for (const auto& parameter : payloadType._parameters)
        {
            rtpMaps.back().parameters.emplace(parameter.first, parameter.second);
        }
    }

    utils::Optional<uint8_t> absSendTimeExtensionId;
    for (const auto& rtpHeaderExtension : channel._rtpHeaderHdrExts)
    {
        if (rtpHeaderExtension._uri.compare("http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time") == 0)
        {
            absSendTimeExtensionId.set(utils::checkedCast<uint8_t>(rtpHeaderExtension._id));
        }
    }

    utils::Optional<uint8_t> audioLevelExtensionId;
    for (const auto& rtpHeaderExtension : channel._rtpHeaderHdrExts)
    {
        if (rtpHeaderExtension._uri.compare("urn:ietf:params:rtp-hdrext:ssrc-audio-level") == 0)
        {
            audioLevelExtensionId.set(utils::checkedCast<uint32_t>(rtpHeaderExtension._id));
        }
    }

    if (absSendTimeExtensionId.isSet())
    {
        for (auto& rtpMap : rtpMaps)
        {
            rtpMap.absSendTimeExtId = absSendTimeExtensionId;
        }
    }

    if (audioLevelExtensionId.isSet())
    {
        for (auto& rtpMap : rtpMaps)
        {
            if (rtpMap.format == RtpMap::Format::OPUS)
            {
                rtpMap.absSendTimeExtId = absSendTimeExtensionId;
            }
        }
    }

    return rtpMaps;
}

std::vector<SimulcastStream> makeSimulcastStreams(const legacyapi::Channel& channel)
{
    std::vector<SimulcastStream> simulcastStreams;
    for (const auto sourcesSsrc : channel._sources)
    {
        auto simulcastGroup = findSimulcastGroup(channel, sourcesSsrc);

        if (simulcastGroup)
        {
            assert(simulcastGroup->_sources.size() > 1);
            const auto sources = simulcastGroup->_sources;
            if (std::find(sources.begin() + 1, sources.end(), sourcesSsrc) != sources.end())
            {
                continue;
            }
        }

        if (simulcastGroup && sourcesSsrc == simulcastGroup->_sources[0])
        {
            SimulcastStream simulcastStream{0};

            for (auto& ssrcAttribute : channel._ssrcAttributes)
            {
                if (ssrcAttribute._content.compare(legacyapi::SsrcAttribute::slidesContent) == 0 &&
                    ssrcAttribute._sources[0] == sourcesSsrc)
                {
                    simulcastStream.contentType = SimulcastStream::VideoContentType::SLIDES;
                }
            }

            for (auto simulcastSsrc : simulcastGroup->_sources)
            {
                const auto feedbackGroup = findFeedbackGroup(channel, simulcastSsrc);
                if (!feedbackGroup)
                {
                    continue;
                }

                simulcastStream.addLevel({simulcastSsrc, feedbackGroup->_sources[1], false});

                logger::debug("Add simulcast level main ssrc %u feedback ssrc %u, content %s, id %s, endpointId %s",
                    "RequestHandlerHelpers",
                    simulcastSsrc,
                    feedbackGroup->_sources[1],
                    toString(simulcastStream.contentType),
                    channel._id.get().c_str(),
                    channel._endpoint.get().c_str());
            }

            simulcastStreams.emplace_back(simulcastStream);
        }
        else
        {
            const auto feedbackGroup = findFeedbackGroup(channel, sourcesSsrc);
            if (!feedbackGroup)
            {
                continue;
            }

            SimulcastStream simulcastStream{0};
            simulcastStream.numLevels = 1;

            simulcastStream.levels[0].ssrc = sourcesSsrc;
            simulcastStream.levels[0].feedbackSsrc = feedbackGroup->_sources[1];

            for (auto& ssrcAttribute : channel._ssrcAttributes)
            {
                if (ssrcAttribute._content.compare(legacyapi::SsrcAttribute::slidesContent) == 0 &&
                    ssrcAttribute._sources[0] == sourcesSsrc)
                {
                    simulcastStream.contentType = SimulcastStream::VideoContentType::SLIDES;
                }
            }

            logger::debug("Add non-simulcast stream main ssrc %u feedback ssrc %u, content %s, id %s, endpointId %s",
                "RequestHandlerHelpers",
                sourcesSsrc,
                feedbackGroup->_sources[1],
                toString(simulcastStream.contentType),
                channel._id.get().c_str(),
                channel._endpoint.get().c_str());

            simulcastStreams.emplace_back(simulcastStream);
        }
    }

    if (simulcastStreams.size() > 2)
    {
        return std::vector<SimulcastStream>(simulcastStreams.end() - 2, simulcastStreams.end());
    }

    return simulcastStreams;
}

} // namespace LegacyApiRequestHandlerHelpers

} // namespace bridge
