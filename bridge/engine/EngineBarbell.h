#pragma once
#include "bridge/BarbellStreamGroupDescription.h"
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
        memory::PacketPoolAllocator& poolAllocator,
        const std::vector<BarbellStreamGroupDescription>& videoDescriptions,
        const std::vector<uint32_t>& audio)
        : id(barbellId),
          ssrcOutboundContexts(128),
          transport(rtcTransport),
          dataChannel(rtcTransport.getLoggableId().getInstanceId(), rtcTransport, poolAllocator),
          videoSsrcMap(16 * 8),
          audioSsrcMap(16)
    {
        for (auto& ssrc : audio)
        {
            audioStreams.push_back(AudioStream{ssrc, utils::Optional<size_t>()});
        }
        for (auto& videoGroup : videoDescriptions)
        {
            SsrcGroup videoStream;
            videoStream.count = videoGroup.ssrcs.size();
            std::memcpy(videoStream.ssrcs, videoGroup.ssrcs.data(), videoStream.count * sizeof(uint32_t));
            std::memcpy(videoStream.feedbackSsrcs,
                videoGroup.feedbackSsrcs.data(),
                videoStream.count * sizeof(uint32_t));

            videoStreams.push_back(videoStream);
        }
    }

    std::string id;
    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext> ssrcOutboundContexts;

    transport::RtcTransport& transport;

    webrtc::WebRtcDataStream dataChannel;
    // some map for ssrc to user id endpointIdHash to be used in activemediaList, that we update from data channel
    // messages

    struct SsrcGroup
    {
        uint32_t ssrcs[3];
        uint32_t feedbackSsrcs[3];
        uint32_t count = 3;

        utils::Optional<size_t> endpointIdHash;
        utils::Optional<std::string> endpointId;
    };

    struct AudioStream
    {
        uint32_t ssrc;
        utils::Optional<size_t> endpointIdHash;
        utils::Optional<std::string> endpointId;
    };

    // inbound ssrcs over barbell and how they group together and map currently to remote endpointId
    std::vector<SsrcGroup> videoStreams;
    std::vector<AudioStream> audioStreams;
    SsrcGroup slideStream;

    concurrency::MpmcHashmap32<uint32_t, SsrcGroup*> videoSsrcMap;
    concurrency::MpmcHashmap32<uint32_t, AudioStream*> audioSsrcMap;
};

} // namespace bridge
