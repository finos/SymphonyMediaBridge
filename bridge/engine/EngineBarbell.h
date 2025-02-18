#pragma once
#include "bridge/BarbellVideoStreamDescription.h"
#include "bridge/RtpMap.h"
#include "bridge/engine/BarbellEndpointMap.h"
#include "bridge/engine/SimulcastStream.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "concurrency/MpmcHashmap.h"
#include "memory/Map.h"
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
        const std::vector<BarbellVideoStreamDescription>& videoDescriptions,
        const std::vector<uint32_t>& audioSsrcs,
        RtpMap& audioRtpMap,
        RtpMap& videoRtpMap,
        RtpMap& videoFeedbackRtpMap);

    utils::Optional<uint32_t> getMainSsrcFor(uint32_t feedbackSsrc);
    utils::Optional<uint32_t> getFeedbackSsrcFor(uint32_t ssrc);
    bool hasVideoDisabled() const { return videoStreams.empty(); }
    static bool isFromBarbell(const char* transportTag) { return 0 == std::strcmp(barbellTag, transportTag); }

    std::string id;
    size_t idHash;
    concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext> ssrcOutboundContexts;

    transport::RtcTransport& transport;
    webrtc::WebRtcDataStream dataChannel;

    // map for ssrc to user id endpointIdHash to be used in activemediaList, that we update from data channel
    // messages
    struct VideoStream
    {
        VideoStream() : stream{0} {}

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

    // inbound ssrcs over barbell and map to endpointId. Static set of streams prepared in constructor
    std::vector<VideoStream> videoStreams;
    std::vector<AudioStream> audioStreams;

    memory::Map<uint32_t, VideoStream*, 128> videoSsrcMap;
    memory::Map<uint32_t, AudioStream*, 16> audioSsrcMap;

    bridge::RtpMap audioRtpMap;
    bridge::RtpMap videoRtpMap;
    bridge::RtpMap videoFeedbackRtpMap;

    uint32_t minClientDownlinkBandwidth;

    static const char* barbellTag;

    struct
    {
        uint64_t timestamp = 0;
        uint64_t count = -1;
    } inboundPackets;
};

} // namespace bridge
