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
        const int32_t audioLevelExtensionId,
        const bool audioMixed,
        const bridge::RtpMap& rtpMap,
        bool ssrcRewrite)
        : _endpointId(endpointId),
          _endpointIdHash(endpointIdHash),
          _localSsrc(localSsrc),
          _remoteSsrc(remoteSsrc),
          _ssrcOutboundContexts(256),
          _transport(transport),
          _audioLevelExtensionId(audioLevelExtensionId),
          _audioMixed(audioMixed),
          _rtpMap(rtpMap),
          _ssrcRewrite(ssrcRewrite)
    {
    }

    std::string _endpointId;
    size_t _endpointIdHash;
    uint32_t _localSsrc;
    utils::Optional<uint32_t> _remoteSsrc;
    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext> _ssrcOutboundContexts;

    transport::RtcTransport& _transport;
    int32_t _audioLevelExtensionId;
    bool _audioMixed;

    bridge::RtpMap _rtpMap;
    bool _ssrcRewrite;
};

} // namespace bridge
