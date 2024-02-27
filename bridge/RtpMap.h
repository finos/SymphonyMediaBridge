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
    static const RtpMap EMPTY;

    enum class Format
    {
        VP8,
        H264,
        RTX,
        OPUS,
        TELEPHONE_EVENT,
        EMPTY
    };

    enum ExtHeaderIdentifiers : uint8_t
    {
        PADDING = 0,
        EOL = 15
    };

    RtpMap() noexcept : format(Format::EMPTY), payloadType(0x7F), sampleRate(0) {}

    explicit RtpMap(const Format format) : format(format)
    {
        switch (format)
        {
        case Format::VP8:
            payloadType = 100;
            sampleRate = 90000;
            break;
        case Format::RTX:
            payloadType = 96;
            sampleRate = 90000;
            break;
        case Format::H264:
            payloadType = 100;
            sampleRate = 90000;
            break;
        case Format::OPUS:
            payloadType = 111;
            sampleRate = 48000;
            channels.set(2);
            break;
        case Format::TELEPHONE_EVENT:
            payloadType = 110;
            sampleRate = 48000;
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
    bool isAudio() const { return format == Format::OPUS || format == Format::TELEPHONE_EVENT; }
    bool isVideo() const { return format == Format::VP8 || format == Format::RTX || format == Format::H264; }

    // @return 15 if none found
    uint8_t suggestAudioLevelExtensionId() const
    {
        if (audioLevelExtId.isSet())
        {
            return audioLevelExtId.get();
        }

        return getFreeExtensionId();
    }

    // @return 15 if none found
    uint8_t getFreeExtensionId() const
    {
        for (uint8_t id = 1; id < ExtHeaderIdentifiers::EOL; ++id)
        {
            if (absSendTimeExtId.valueOr(16) != id && c9infoExtId.valueOr(16) != id &&
                audioLevelExtId.valueOr(16) != id)
            {
                return id;
            }
        }
        return ExtHeaderIdentifiers::EOL;
    }

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
