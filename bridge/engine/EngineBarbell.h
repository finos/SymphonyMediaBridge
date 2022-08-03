#pragma once
#include "bridge/BarbellStreamGroupDescription.h"
#include "bridge/RtpMap.h"
#include "bridge/engine/BarbellEndpointMap.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "concurrency/MpmcHashmap.h"
#include "memory/StackMap.h"
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
        const std::vector<BarbellStreamGroupDescription>& videoDescriptions,
        const std::vector<uint32_t>& audio,
        RtpMap& audioRtpMap,
        RtpMap& videoRtpMap,
        RtpMap& videoFeedbackRtpMap)
        : id(barbellId),
          idHash(utils::hash<std::string>{}(barbellId)),
          ssrcOutboundContexts(128),
          transport(rtcTransport),
          dataChannel(rtcTransport.getLoggableId().getInstanceId(), rtcTransport),
          audioRtpMap(audioRtpMap),
          videoRtpMap(videoRtpMap),
          videoFeedbackRtpMap(videoFeedbackRtpMap)
    {
        for (auto& ssrc : audio)
        {
            audioStreams.push_back(AudioStream{ssrc, utils::Optional<size_t>()});
            audioSsrcMap.emplace(ssrc, &audioStreams.back());
        }

        for (auto& videoGroup : videoDescriptions)
        {
            VideoStream videoStream;
            videoStream.stream._contentType = (videoGroup.slides ? SimulcastStream::VideoContentType::SLIDES
                                                                 : SimulcastStream::VideoContentType::VIDEO);
            videoStream.stream._numLevels = videoGroup.ssrcLevels.size();

            for (size_t i = 0; i < videoStream.stream._numLevels; ++i)
            {
                videoStream.stream._levels[i]._ssrc = videoGroup.ssrcLevels[i].ssrc;
                videoStream.stream._levels[i]._feedbackSsrc = videoGroup.ssrcLevels[i].feedbackSsrc;
            }

            VideoStream* addedStream = nullptr;
            if (videoGroup.slides)
            {
                slideStream = videoStream;
                addedStream = &slideStream;
            }
            else
            {
                videoStreams.push_back(videoStream);
                addedStream = &videoStreams.back();
            }

            for (size_t i = 0; addedStream && i < videoStream.stream._numLevels; ++i)
            {
                videoSsrcMap.emplace(videoStream.stream._levels[i]._ssrc, addedStream);
                videoSsrcMap.emplace(videoStream.stream._levels[i]._feedbackSsrc, addedStream);
            }
        }
    }

    utils::Optional<uint32_t> getMainSsrcFor(uint32_t feedbackSsrc)
    {
        auto videoStream = videoSsrcMap.getItem(feedbackSsrc);
        if (!videoStream)
        {
            return utils::Optional<uint32_t>();
        }

        return videoStream->stream.getMainSsrcFor(feedbackSsrc);
    }

    utils::Optional<uint32_t> getFeedbackSsrcFor(uint32_t ssrc)
    {
        auto videoStream = videoSsrcMap.getItem(ssrc);
        if (!videoStream)
        {
            return utils::Optional<uint32_t>();
        }

        return videoStream->stream.getFeedbackSsrcFor(ssrc);
    }

    std::string id;
    size_t idHash;
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

    memory::StackMap<uint32_t, VideoStream*, 32> videoSsrcMap;
    memory::StackMap<uint32_t, AudioStream*, 16> audioSsrcMap;

    bridge::RtpMap audioRtpMap;
    bridge::RtpMap videoRtpMap;
    bridge::RtpMap videoFeedbackRtpMap;
};

} // namespace bridge
