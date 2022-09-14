#pragma once

#include "bridge/engine/SimulcastLevel.h"
#include "utils/Optional.h"
#include <cstddef>
#include <cstdint>

namespace bridge
{

/**
 * For simplicity, all video streams are of type SimulcastStream in smb, even if they send only one level and do not
 * have a SIM group. SimulcastStream entries with _numLevels set to 1 is a single non-simulcast video stream,
 * and entries with _numLevels > 1 are simulcast video streams with a matching SIM group.
 */
struct SimulcastStream
{
    static constexpr size_t maxLevels = 3;

    enum class VideoContentType
    {
        VIDEO = 0,
        SLIDES = 1
    };

    utils::Optional<uint32_t> getFeedbackSsrcFor(uint32_t ssrc)
    {
        for (size_t i = 0; i < numLevels; ++i)
        {
            if (levels[i].ssrc == ssrc)
            {
                return utils::Optional<uint32_t>(levels[i].feedbackSsrc);
            }
        }
        return utils::Optional<uint32_t>();
    }

    utils::Optional<uint32_t> getMainSsrcFor(uint32_t feedbackSsrc)
    {
        for (size_t i = 0; i < numLevels; ++i)
        {
            if (levels[i].feedbackSsrc == feedbackSsrc)
            {
                return utils::Optional<uint32_t>(levels[i].ssrc);
            }
        }
        return utils::Optional<uint32_t>();
    }

    bool isSendingVideo() const { return numLevels > 0 && contentType == VideoContentType::VIDEO; }
    bool isSendingSlides() const { return numLevels > 0 && contentType == VideoContentType::SLIDES; }

    utils::Optional<uint32_t> getLevelOf(uint32_t ssrc) const
    {
        for (size_t i = 0; i < numLevels; ++i)
        {
            if (levels[i].ssrc == ssrc || levels[i].feedbackSsrc == ssrc)
            {
                return utils::Optional<uint32_t>(i);
            }
        }

        return utils::Optional<uint32_t>();
    }

    size_t numLevels;
    size_t highestActiveLevel;
    SimulcastLevel levels[maxLevels];
    VideoContentType contentType;
};

inline bool operator==(const SimulcastStream& first, const SimulcastStream& second)
{
    if (first.numLevels != second.numLevels)
    {
        return false;
    }

    for (size_t i = 0; i < first.numLevels; ++i)
    {
        if (first.levels[i].ssrc != second.levels[i].ssrc ||
            first.levels[i].feedbackSsrc != second.levels[i].feedbackSsrc)
        {
            return false;
        }
    }

    return true;
}

inline const char* toString(const SimulcastStream::VideoContentType videoContentType)
{
    switch (videoContentType)
    {
    case SimulcastStream::VideoContentType::VIDEO:
        return "video";
    case SimulcastStream::VideoContentType::SLIDES:
        return "slides";
    default:
        return "invalid!";
    }
}

} // namespace bridge
