#pragma once

#include "bridge/engine/SimulcastLevel.h"
#include "utils/Optional.h"
#include "utils/Span.h"
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
    constexpr static size_t maxLevels = 3;

    enum class VideoContentType
    {
        VIDEO = 0,
        SLIDES = 1
    };

    utils::Optional<uint32_t> getFeedbackSsrcFor(uint32_t ssrc)
    {
        for (auto& simulcastLevel : getLevels())
        {
            if (simulcastLevel.ssrc == ssrc)
            {
                return utils::Optional<uint32_t>(simulcastLevel.feedbackSsrc);
            }
        }
        return utils::Optional<uint32_t>();
    }

    utils::Optional<uint32_t> getMainSsrcFor(uint32_t feedbackSsrc)
    {
        for (auto& simulcastLevel : getLevels())
        {
            if (simulcastLevel.feedbackSsrc == feedbackSsrc)
            {
                return utils::Optional<uint32_t>(simulcastLevel.ssrc);
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

    uint32_t getKeySsrc() const { return levels[0].ssrc; }
    utils::Span<SimulcastLevel> getLevels()
    {
        return utils::Span<SimulcastLevel>(levels, numLevels > maxLevels ? maxLevels : numLevels);
    }
    utils::Span<const SimulcastLevel> getLevels() const
    {
        return utils::Span<const SimulcastLevel>(levels, numLevels > maxLevels ? maxLevels : numLevels);
    }

    void addLevel(const SimulcastLevel& level)
    {
        if (numLevels >= maxLevels)
        {
            return;
        }
        levels[numLevels++] = level;
    }

    void reset()
    {
        numLevels = 0;
        highestActiveLevel = 0;
        contentType = VideoContentType::VIDEO;
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
