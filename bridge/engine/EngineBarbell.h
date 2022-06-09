#pragma once
#include "bridge/RtpMap.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "concurrency/MpmcHashmap.h"
#include "webrtc/WebRtcDataStream.h"
#include <cstdint>

namespace transport
{
class RtcTransport;
}

namespace bridge
{

struct EngineBarbell
{
    EngineBarbell(const std::string& barbellId,
        transport::RtcTransport& rtcTransport,
        memory::PacketPoolAllocator& poolAllocator)
        : id(barbellId),
          ssrcOutboundContexts(128),
          transport(rtcTransport),
          dataChannel(rtcTransport.getLoggableId().getInstanceId(), rtcTransport, poolAllocator)
    {
    }

    std::string id;
    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext> ssrcOutboundContexts;

    transport::RtcTransport& transport;

    webrtc::WebRtcDataStream dataChannel;
    // some map for ssrc to user id endpointIdHash to be used in activemediaList, that we update from data channel
    // messages
};

} // namespace bridge
