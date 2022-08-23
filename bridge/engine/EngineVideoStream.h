#pragma once
#include "bridge/RtpMap.h"
#include "bridge/engine/UntypedEngineObject.h"
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

struct EngineVideoStream final : UntypedEngineObject
{
    EngineVideoStream(const std::string& endpointId,
        const size_t endpointIdHash,
        const uint32_t localSsrc,
        const SimulcastStream& simulcastStream,
        const utils::Optional<SimulcastStream>& secondarySimulcastStream,
        transport::RtcTransport& transport,
        const bridge::RtpMap& rtpMap,
        const bridge::RtpMap& feedbackRtpMap,
        const SsrcWhitelist& whitelist,
        const bool ssrcRewrite,
        const std::vector<api::SsrcPair>& pinSsrcs)
        : endpointId(endpointId),
          endpointIdHash(endpointIdHash),
          localSsrc(localSsrc),
          simulcastStream(simulcastStream),
          secondarySimulcastStream(secondarySimulcastStream),
          ssrcOutboundContexts(1024),
          transport(transport),
          rtpMap(rtpMap),
          feedbackRtpMap(feedbackRtpMap),
          ssrcRewrite(ssrcRewrite),
          videoPinSsrcs(SsrcRewrite::ssrcArraySize)
    {
        assert(pinSsrcs.size() <= SsrcRewrite::ssrcArraySize);

        std::memcpy(&ssrcWhitelist, &whitelist, sizeof(SsrcWhitelist));
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

    std::string endpointId;
    size_t endpointIdHash;
    uint32_t localSsrc;
    SimulcastStream simulcastStream;
    utils::Optional<SimulcastStream> secondarySimulcastStream;
    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext> ssrcOutboundContexts;

    transport::RtcTransport& transport;

    bridge::RtpMap rtpMap;
    bridge::RtpMap feedbackRtpMap;

    SsrcWhitelist ssrcWhitelist;
    bool ssrcRewrite;

    concurrency::MpmcQueue<SimulcastLevel> videoPinSsrcs;
    utils::Optional<SimulcastLevel> pinSsrc;
};

} // namespace bridge
