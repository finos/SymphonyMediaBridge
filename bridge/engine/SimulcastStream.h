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
        for (size_t i = 0; i < _numLevels; ++i)
        {
            if (_levels[i]._ssrc == ssrc)
            {
                return utils::Optional<uint32_t>(_levels[i]._feedbackSsrc);
            }
        }
        return utils::Optional<uint32_t>();
    }

    utils::Optional<uint32_t> getMainSsrcFor(uint32_t feedbackSsrc)
    {
        for (size_t i = 0; i < _numLevels; ++i)
        {
            if (_levels[i]._feedbackSsrc == feedbackSsrc)
            {
                return utils::Optional<uint32_t>(_levels[i]._ssrc);
            }
        }
        return utils::Optional<uint32_t>();
    }

    bool isSendingVideo() const { return _numLevels > 0 && _contentType == VideoContentType::VIDEO; }
    bool isSendingSlides() const { return _numLevels > 0 && _contentType == VideoContentType::SLIDES; }

    uint32_t getLevelOf(uint32_t ssrc) const
    {
        for (size_t i = 0; i < _numLevels; ++i)
        {
            if (_levels[i]._ssrc == ssrc)
            {
                return i;
            }
        }

        return 0;
    }

    size_t _numLevels;
    size_t _highestActiveLevel;
    SimulcastLevel _levels[maxLevels];
    VideoContentType _contentType;
};

inline bool operator==(const SimulcastStream& first, const SimulcastStream& second)
{
    if (first._numLevels != second._numLevels)
    {
        return false;
    }

    for (size_t i = 0; i < first._numLevels; ++i)
    {
        if (first._levels[i]._ssrc != second._levels[i]._ssrc ||
            first._levels[i]._feedbackSsrc != second._levels[i]._feedbackSsrc)
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
