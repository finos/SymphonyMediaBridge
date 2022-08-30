#pragma once

#include "bridge/RtpMap.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "concurrency/MpmcHashmap.h"
#include <cstdint>

namespace transport
{
class RtcTransport;
}

namespace bridge
{

struct EngineAudioStream
{
    EngineAudioStream(const std::string& endpointId,
        const size_t endpointIdHash,
        const uint32_t localSsrc,
        const utils::Optional<uint32_t>& remoteSsrc,
        transport::RtcTransport& transport,
        const bool audioMixed,
        const bridge::RtpMap& rtpMap,
        bool ssrcRewrite)
        : endpointId(endpointId),
          endpointIdHash(endpointIdHash),
          localSsrc(localSsrc),
          remoteSsrc(remoteSsrc),
          ssrcOutboundContexts(256),
          transport(transport),
          audioMixed(audioMixed),
          rtpMap(rtpMap),
          ssrcRewrite(ssrcRewrite)
    {
    }

    std::string endpointId;
    size_t endpointIdHash;
    uint32_t localSsrc;
    utils::Optional<uint32_t> remoteSsrc;
    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext> ssrcOutboundContexts;

    transport::RtcTransport& transport;
    bool audioMixed;

    bridge::RtpMap rtpMap;
    bool ssrcRewrite;
};

} // namespace bridge
