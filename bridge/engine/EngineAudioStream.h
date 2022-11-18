#pragma once
#include "bridge/RtpMap.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "concurrency/MpmcHashmap.h"
#include "memory/Map.h"
#include "utils/StdExtensions.h"
#include <cstdint>

namespace transport
{
class RtcTransport;
}

namespace bridge
{

struct EngineAudioStream
{
    static constexpr uint32_t MAX_NEIGHBOUR_COUNT = 128;

    EngineAudioStream(const std::string& endpointId_,
        const size_t endpointIdHash_,
        const uint32_t localSsrc_,
        const utils::Optional<uint32_t>& remoteSsrc_,
        transport::RtcTransport& transport_,
        const bool audioMixed_,
        const bridge::RtpMap& rtpMap_,
        bool ssrcRewrite_,
        const uint32_t idleTimeoutSeconds_,
        const std::vector<uint32_t>& neighbourList)
        : endpointId(endpointId_),
          endpointIdHash(endpointIdHash_),
          localSsrc(localSsrc_),
          remoteSsrc(remoteSsrc_),
          ssrcOutboundContexts(256),
          transport(transport_),
          audioMixed(audioMixed_),
          rtpMap(rtpMap_),
          ssrcRewrite(ssrcRewrite_),
          idleTimeoutSeconds(idleTimeoutSeconds_),
          createdAt(utils::Time::getAbsoluteTime())
    {
        for (auto& neighbour : neighbourList)
        {
            neighbours.add(neighbour, true);
        }
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

    memory::Map<uint32_t, bool, MAX_NEIGHBOUR_COUNT> neighbours;
};

} // namespace bridge
