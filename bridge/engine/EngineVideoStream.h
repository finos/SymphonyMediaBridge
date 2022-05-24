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
    EngineVideoStream(const std::string& endpointId,
        const size_t endpointIdHash,
        const uint32_t localSsrc,
        const SimulcastStream& simulcastStream,
        const utils::Optional<SimulcastStream>& secondarySimulcastStream,
        transport::RtcTransport& transport,
        const bridge::RtpMap& rtpMap,
        const bridge::RtpMap& feedbackRtpMap,
        const SsrcWhitelist& ssrcWhitelist,
        const bool ssrcRewrite,
        const std::vector<SimulcastLevel>& videoPinSsrcs)
        : _endpointId(endpointId),
          _endpointIdHash(endpointIdHash),
          _localSsrc(localSsrc),
          _simulcastStream(simulcastStream),
          _secondarySimulcastStream(secondarySimulcastStream),
          _ssrcOutboundContexts(1024),
          _transport(transport),
          _rtpMap(rtpMap),
          _feedbackRtpMap(feedbackRtpMap),
          _ssrcRewrite(ssrcRewrite),
          _videoPinSsrcs(SsrcRewrite::ssrcArraySize)
    {
        assert(videoPinSsrcs.size() <= SsrcRewrite::ssrcArraySize);

        std::memcpy(&_ssrcWhitelist, &ssrcWhitelist, sizeof(SsrcWhitelist));
        for (const auto& videoSsrc : videoPinSsrcs)
        {
            _videoPinSsrcs.push(videoSsrc);
        }
    }

    utils::Optional<uint32_t> getFeedbackSsrcFor(uint32_t ssrc)
    {
        auto fbSsrc = _simulcastStream.getFeedbackSsrcFor(ssrc);
        if (!fbSsrc.isSet() && _secondarySimulcastStream.isSet())
        {
            return _secondarySimulcastStream.get().getFeedbackSsrcFor(ssrc);
        }
        return fbSsrc;
    }

    utils::Optional<uint32_t> getMainSsrcFor(uint32_t feedbackSsrc)
    {
        auto mainSsrc = _simulcastStream.getMainSsrcFor(feedbackSsrc);
        if (!mainSsrc.isSet() && _secondarySimulcastStream.isSet())
        {
            return _secondarySimulcastStream.get().getMainSsrcFor(feedbackSsrc);
        }
        return mainSsrc;
    }

    std::string _endpointId;
    size_t _endpointIdHash;
    uint32_t _localSsrc;
    SimulcastStream _simulcastStream;
    utils::Optional<SimulcastStream> _secondarySimulcastStream;
    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext> _ssrcOutboundContexts;

    transport::RtcTransport& _transport;

    bridge::RtpMap _rtpMap;
    bridge::RtpMap _feedbackRtpMap;

    SsrcWhitelist _ssrcWhitelist;
    bool _ssrcRewrite;

    concurrency::MpmcQueue<SimulcastLevel> _videoPinSsrcs;
    utils::Optional<SimulcastLevel> _pinSsrc;
};

} // namespace bridge
