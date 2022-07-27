#pragma once
#include "bridge/BarbellStreamGroupDescription.h"
#include "bridge/RtpMap.h"
#include "bridge/engine/BarbellEndpointMap.h"
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
            VideoStream videoStream;
            videoStream.stream._numLevels = videoGroup.ssrcs.size();

            for (size_t i = 0; i < videoStream.stream._numLevels; ++i)
            {
                videoStream.stream._levels[i]._ssrc = videoGroup.ssrcs[i];
                videoStream.stream._levels[i]._feedbackSsrc = videoGroup.feedbackSsrcs[i];
            }

            videoStreams.push_back(videoStream);
        }
    }

    std::string id;
    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext> ssrcOutboundContexts;

    transport::RtcTransport& transport;

    webrtc::WebRtcDataStream dataChannel;

    // map for ssrc to user id endpointIdHash to be used in activemediaList, that we update from data channel
    // messages
    struct VideoStream
    {
        SimulcastStream stream;

        utils::Optional<size_t> endpointIdHash;
        utils::Optional<EndpointIdString> endpointId;
    };

    struct AudioStream
    {
        uint32_t ssrc;
        utils::Optional<size_t> endpointIdHash;
        utils::Optional<EndpointIdString> endpointId;
    };

    // inbound ssrcs over barbell and how they group together and map currently to remote endpointId
    std::vector<VideoStream> videoStreams;
    std::vector<AudioStream> audioStreams;
    VideoStream slideStream;

    concurrency::MpmcHashmap32<uint32_t, VideoStream*> videoSsrcMap;
    concurrency::MpmcHashmap32<uint32_t, AudioStream*> audioSsrcMap;
};

} // namespace bridge
