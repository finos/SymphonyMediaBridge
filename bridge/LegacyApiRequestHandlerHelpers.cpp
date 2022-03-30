#include "bridge/LegacyApiRequestHandlerHelpers.h"
#include "bridge/RtpMap.h"
#include "bridge/engine/SimulcastStream.h"
#include "legacyapi/Channel.h"
#include "legacyapi/SsrcGroup.h"
#include "logger/Logger.h"
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
            rtpMaps.emplace_back(RtpMap::opus());
        }
        else if (payloadType._name.compare("VP8") == 0)
        {
            rtpMaps.emplace_back(RtpMap::vp8());
        }
        else if (payloadType._name.compare("rtx") == 0)
        {
            rtpMaps.emplace_back(RtpMap(RtpMap::Format::VP8RTX, 96, 90000));
        }

        for (const auto& parameter : payloadType._parameters)
        {
            rtpMaps.back()._parameters.emplace(parameter.first, parameter.second);
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
                    simulcastStream._contentType = SimulcastStream::VideoContentType::SLIDES;
                }
            }

            for (auto simulcastSsrc : simulcastGroup->_sources)
            {
                const auto feedbackGroup = findFeedbackGroup(channel, simulcastSsrc);
                if (!feedbackGroup)
                {
                    continue;
                }

                simulcastStream._levels[simulcastStream._numLevels]._ssrc = simulcastSsrc;
                simulcastStream._levels[simulcastStream._numLevels]._feedbackSsrc = feedbackGroup->_sources[1];
                ++simulcastStream._numLevels;

                logger::debug("Add simulcast level main ssrc %u feedback ssrc %u, content %s, id %s, endpointId %s",
                    "RequestHandlerHelpers",
                    simulcastSsrc,
                    feedbackGroup->_sources[1],
                    toString(simulcastStream._contentType),
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
            simulcastStream._numLevels = 1;

            simulcastStream._levels[0]._ssrc = sourcesSsrc;
            simulcastStream._levels[0]._feedbackSsrc = feedbackGroup->_sources[1];

            for (auto& ssrcAttribute : channel._ssrcAttributes)
            {
                if (ssrcAttribute._content.compare(legacyapi::SsrcAttribute::slidesContent) == 0 &&
                    ssrcAttribute._sources[0] == sourcesSsrc)
                {
                    simulcastStream._contentType = SimulcastStream::VideoContentType::SLIDES;
                }
            }

            logger::debug("Add non-simulcast stream main ssrc %u feedback ssrc %u, content %s, id %s, endpointId %s",
                "RequestHandlerHelpers",
                sourcesSsrc,
                feedbackGroup->_sources[1],
                toString(simulcastStream._contentType),
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
