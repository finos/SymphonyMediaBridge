#include "EngineBarbell.h"
#include "transport/RtcTransport.h"

namespace bridge
{
const char* EngineBarbell::barbellTag = "BB";

EngineBarbell::EngineBarbell(const std::string& barbellId,
    transport::RtcTransport& rtcTransport,
    const std::vector<BarbellVideoStreamDescription>& videoDescriptions,
    const std::vector<uint32_t>& audioSsrcs,
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
      videoFeedbackRtpMap(videoFeedbackRtpMap),
      minClientDownlinkBandwidth(100000)
{
    audioStreams.reserve(audioSsrcs.size());
    for (auto& ssrc : audioSsrcs)
    {
        audioStreams.push_back(AudioStream{ssrc, utils::Optional<size_t>()});
    }

    videoStreams.reserve(videoDescriptions.size());
    for (auto& videoGroup : videoDescriptions)
    {
        VideoStream videoStream;
        videoStream.stream.contentType =
            (videoGroup.slides ? SimulcastStream::VideoContentType::SLIDES : SimulcastStream::VideoContentType::VIDEO);

        for (auto& ssrcPair : videoGroup.ssrcLevels)
        {
            videoStream.stream.levels[videoStream.stream.numLevels++] = {ssrcPair.main, ssrcPair.feedback, false};
        }

        if (videoGroup.slides)
        {
            slideStream = videoStream;
        }
        else
        {
            videoStreams.push_back(videoStream);
        }
    }

    // setup lookup table after vector population, otherwise objects may move due to reallocation
    for (auto& audioStream : audioStreams)
    {
        audioSsrcMap.emplace(audioStream.ssrc, &audioStream);
    }

    for (auto& videoStream : videoStreams)
    {
        for (size_t i = 0; i < videoStream.stream.numLevels; ++i)
        {
            auto& ssrcPair = videoStream.stream.levels[i];
            videoSsrcMap.emplace(ssrcPair.ssrc, &videoStream);
            videoSsrcMap.emplace(ssrcPair.feedbackSsrc, &videoStream);
        }
    }

    for (size_t i = 0; i < slideStream.stream.numLevels; ++i)
    {
        auto& ssrcPair = slideStream.stream.levels[i];
        videoSsrcMap.emplace(ssrcPair.ssrc, &slideStream);
        videoSsrcMap.emplace(ssrcPair.feedbackSsrc, &slideStream);
    }
}

utils::Optional<uint32_t> EngineBarbell::getMainSsrcFor(uint32_t feedbackSsrc)
{
    auto videoStream = videoSsrcMap.getItem(feedbackSsrc);
    if (!videoStream)
    {
        return utils::Optional<uint32_t>();
    }

    return videoStream->stream.getMainSsrcFor(feedbackSsrc);
}

utils::Optional<uint32_t> EngineBarbell::getFeedbackSsrcFor(uint32_t ssrc)
{
    auto videoStream = videoSsrcMap.getItem(ssrc);
    if (!videoStream)
    {
        return utils::Optional<uint32_t>();
    }

    return videoStream->stream.getFeedbackSsrcFor(ssrc);
}

} // namespace bridge
