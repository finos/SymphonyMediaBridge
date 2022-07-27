#include "EngineBarbell.h"
#include "transport/RtcTransport.h"

namespace bridge
{
const char* EngineBarbell::barbellTag = "BB";

EngineBarbell::EngineBarbell(const std::string& barbellId,
    transport::RtcTransport& rtcTransport,
    const std::vector<BarbellStreamGroupDescription>& videoDescriptions,
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
      videoFeedbackRtpMap(videoFeedbackRtpMap)
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
        videoStream.stream._contentType =
            (videoGroup.slides ? SimulcastStream::VideoContentType::SLIDES : SimulcastStream::VideoContentType::VIDEO);
        videoStream.stream._numLevels = videoGroup.ssrcLevels.size();

        for (size_t i = 0; i < videoStream.stream._numLevels; ++i)
        {
            videoStream.stream._levels[i]._ssrc = videoGroup.ssrcLevels[i].ssrc;
            videoStream.stream._levels[i]._feedbackSsrc = videoGroup.ssrcLevels[i].feedbackSsrc;
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
        for (size_t i = 0; i < videoStream.stream._numLevels; ++i)
        {
            videoSsrcMap.emplace(videoStream.stream._levels[i]._ssrc, &videoStream);
            videoSsrcMap.emplace(videoStream.stream._levels[i]._feedbackSsrc, &videoStream);
        }
    }

    for (size_t i = 0; i < slideStream.stream._numLevels; ++i)
    {
        videoSsrcMap.emplace(slideStream.stream._levels[i]._ssrc, &slideStream);
        videoSsrcMap.emplace(slideStream.stream._levels[i]._feedbackSsrc, &slideStream);
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
