#pragma once
#include "utils/Optional.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bridge
{

struct RtpMap
{
    enum class Format
    {
        VP8,
        VP8RTX,
        OPUS,
        EMPTY
    };

    RtpMap() : format(Format::EMPTY), payloadType(4096), sampleRate(0) {}

    explicit RtpMap(const Format format) : format(format)
    {
        switch (format)
        {
        case Format::VP8:
            payloadType = 100;
            sampleRate = 90000;
            break;
        case Format::VP8RTX:
            payloadType = 96;
            sampleRate = 90000;
            break;
        case Format::OPUS:
            payloadType = 111;
            sampleRate = 48000;
            channels.set(2);
            break;
        default:
            assert(false);
            payloadType = 4096;
            sampleRate = 0;
            break;
        }
    }

    RtpMap(const Format format, const uint16_t payloadType, const uint32_t sampleRate)
        : format(format),
          payloadType(payloadType),
          sampleRate(sampleRate)
    {
    }

    RtpMap(const Format format,
        const uint16_t payloadType,
        const uint32_t sampleRate,
        const utils::Optional<uint32_t>& channels)
        : format(format),
          payloadType(payloadType),
          sampleRate(sampleRate),
          channels(channels)
    {
    }

    RtpMap(const RtpMap& rtpMap) = default;

    bool isEmpty() const { return format == Format::EMPTY; }

    Format format;
    uint16_t payloadType;
    uint32_t sampleRate;
    utils::Optional<uint32_t> channels;
    std::unordered_map<std::string, std::string> parameters;
    std::vector<std::pair<std::string, utils::Optional<std::string>>> rtcpFeedbacks;
    utils::Optional<uint8_t> audioLevelExtId;
    utils::Optional<uint8_t> absSendTimeExtId;
    utils::Optional<uint8_t> c9infoExtId;
};

} // namespace bridge
