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
        bool ssrcRewrite,
        const uint32_t idleTimeoutSeconds)
        : endpointId(endpointId),
          endpointIdHash(endpointIdHash),
          localSsrc(localSsrc),
          remoteSsrc(remoteSsrc),
          ssrcOutboundContexts(256),
          transport(transport),
          audioMixed(audioMixed),
          rtpMap(rtpMap),
          ssrcRewrite(ssrcRewrite),
          idleTimeoutSeconds(idleTimeoutSeconds),
          createdAt(utils::Time::getAbsoluteTime())
    {
    }

    const std::string endpointId;
    const size_t endpointIdHash;
    const uint32_t localSsrc;
    utils::Optional<uint32_t> remoteSsrc;
    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext> ssrcOutboundContexts;

    transport::RtcTransport& transport;
    const bool audioMixed;

    bridge::RtpMap rtpMap;
    const bool ssrcRewrite;
    const uint32_t idleTimeoutSeconds;
    const uint64_t createdAt;
};

} // namespace bridge
