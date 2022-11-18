#pragma once
#include "bridge/RtpMap.h"
#include "bridge/engine/SimulcastLevel.h"
#include "bridge/engine/SimulcastStream.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "bridge/engine/SsrcRewrite.h"
#include "bridge/engine/SsrcWhitelist.h"
#include "concurrency/MpmcHashmap.h"
#include "utils/Optional.h"
#include <cstdint>
#include <string>
#include <vector>

namespace transport
{
class RtcTransport;
}

namespace bridge
{

struct EngineVideoStream
{
    EngineVideoStream(const std::string& endpointId_,
        const size_t endpointIdHash_,
        const uint32_t localSsrc_,
        const SimulcastStream& simulcastStream_,
        const utils::Optional<SimulcastStream>& secondarySimulcastStream_,
        transport::RtcTransport& transport_,
        const bridge::RtpMap& rtpMap_,
        const bridge::RtpMap& feedbackRtpMap_,
        const SsrcWhitelist& whitelist_,
        const bool ssrcRewrite_,
        const std::vector<api::SsrcPair>& pinSsrcs,
        const uint32_t idleTimeoutSeconds_)
        : endpointId(endpointId_),
          endpointIdHash(endpointIdHash_),
          localSsrc(localSsrc_),
          simulcastStream(simulcastStream_),
          secondarySimulcastStream(secondarySimulcastStream_),
          ssrcOutboundContexts(1024),
          transport(transport_),
          rtpMap(rtpMap_),
          feedbackRtpMap(feedbackRtpMap_),
          ssrcWhitelist(whitelist_),
          ssrcRewrite(ssrcRewrite_),
          videoPinSsrcs(SsrcRewrite::ssrcArraySize),
          idleTimeoutSeconds(idleTimeoutSeconds_),
          createdAt(utils::Time::getAbsoluteTime())
    {
        assert(pinSsrcs.size() <= SsrcRewrite::ssrcArraySize);

        for (const auto& videoSsrc : pinSsrcs)
        {
            videoPinSsrcs.push({videoSsrc.main, videoSsrc.feedback, false});
        }
    }

    utils::Optional<uint32_t> getFeedbackSsrcFor(uint32_t ssrc)
    {
        auto fbSsrc = simulcastStream.getFeedbackSsrcFor(ssrc);
        if (!fbSsrc.isSet() && secondarySimulcastStream.isSet())
        {
            return secondarySimulcastStream.get().getFeedbackSsrcFor(ssrc);
        }
        return fbSsrc;
    }

    utils::Optional<uint32_t> getMainSsrcFor(uint32_t feedbackSsrc)
    {
        auto mainSsrc = simulcastStream.getMainSsrcFor(feedbackSsrc);
        if (!mainSsrc.isSet() && secondarySimulcastStream.isSet())
        {
            return secondarySimulcastStream.get().getMainSsrcFor(feedbackSsrc);
        }
        return mainSsrc;
    }

    const std::string endpointId;
    const size_t endpointIdHash;
    const uint32_t localSsrc;
    SimulcastStream simulcastStream;
    utils::Optional<SimulcastStream> secondarySimulcastStream;
    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext> ssrcOutboundContexts;

    transport::RtcTransport& transport;

    const bridge::RtpMap rtpMap;
    const bridge::RtpMap feedbackRtpMap;

    SsrcWhitelist ssrcWhitelist;
    const bool ssrcRewrite;

    concurrency::MpmcQueue<SimulcastLevel> videoPinSsrcs;
    utils::Optional<SimulcastLevel> pinSsrc;
    const uint32_t idleTimeoutSeconds;
    const uint64_t createdAt;
};

} // namespace bridge
