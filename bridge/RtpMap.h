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

    RtpMap() : _format(Format::EMPTY), _payloadType(0xFF), _sampleRate(0) {}

    explicit RtpMap(const Format format) : _format(format)
    {
        switch (format)
        {
        case Format::VP8:
            _payloadType = 100;
            _sampleRate = 90000;
            break;
        case Format::VP8RTX:
            _payloadType = 96;
            _sampleRate = 90000;
            break;
        case Format::OPUS:
            _payloadType = 111;
            _sampleRate = 48000;
            _channels.set(2);
            break;
        default:
            assert(false);
            _payloadType = 0xFF;
            _sampleRate = 0;
            break;
        }
    }

    RtpMap(const Format format, const uint8_t payloadType, const uint32_t sampleRate)
        : _format(format),
          _payloadType(payloadType),
          _sampleRate(sampleRate)
    {
    }

    RtpMap(const Format format,
        const uint16_t payloadType,
        const uint8_t sampleRate,
        const utils::Optional<uint32_t>& channels)
        : _format(format),
          _payloadType(payloadType),
          _sampleRate(sampleRate),
          _channels(channels)
    {
    }

    RtpMap(const RtpMap& rtpMap) = default;

    bool isEmpty() const { return _format == Format::EMPTY; }

    Format _format;
    uint8_t _payloadType;
    uint32_t _sampleRate;
    utils::Optional<uint32_t> _channels;
    std::unordered_map<std::string, std::string> _parameters;
    std::vector<std::pair<std::string, utils::Optional<std::string>>> _rtcpFeedbacks;
    utils::Optional<uint8_t> _audioLevelExtId;
    utils::Optional<uint8_t> _absSendTimeExtId;
    utils::Optional<uint8_t> _c9infoExtId;
};

} // namespace bridge
