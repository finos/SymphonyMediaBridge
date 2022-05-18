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
        PCMU = 0,
        PCMA = 8,
        VP8 = 100,
        VP8RTX = 96,
        OPUS = 111,
        EMPTY = 4096
    };

    RtpMap() : format(Format::EMPTY), payloadType(0x7F), sampleRate(0) {}

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
            payloadType = 0x7F;
            sampleRate = 0;
            break;
        }
    }

    RtpMap(const Format format, const uint8_t payloadType, const uint32_t sampleRate)
        : format(format),
          payloadType(payloadType),
          sampleRate(sampleRate)
    {
        assert(payloadType <= 0x7F);
    }

    RtpMap(const Format format,
        const uint8_t payloadType,
        const uint32_t sampleRate,
        const utils::Optional<uint32_t>& channels)
        : format(format),
          payloadType(payloadType),
          sampleRate(sampleRate),
          channels(channels)
    {
        assert(payloadType <= 0x7F);
    }

    RtpMap(const RtpMap& rtpMap) = default;

    bool isEmpty() const { return format == Format::EMPTY; }
    bool isAudio() const { return format == Format::OPUS; }
    bool isVideo() const { return format == Format::VP8 || format == Format::VP8RTX; }

    Format format;
    uint8_t payloadType;
    uint32_t sampleRate;
    utils::Optional<uint32_t> channels;
    std::unordered_map<std::string, std::string> parameters;
    std::vector<std::pair<std::string, utils::Optional<std::string>>> rtcpFeedbacks;
    utils::Optional<uint8_t> audioLevelExtId;
    utils::Optional<uint8_t> absSendTimeExtId;
    utils::Optional<uint8_t> c9infoExtId;
};

} // namespace bridge
