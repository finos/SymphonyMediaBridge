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
    enum class Format : uint16_t
    {
        VP8 = 100,
        VP8RTX = 96,
        OPUS = 111,
        EMPTY = 4096
    };

    static const RtpMap& opus();
    static const RtpMap& vp8();

    RtpMap() : _format(Format::EMPTY), _payloadType(4096), _sampleRate(0) {}

    RtpMap(const Format format, const uint32_t payloadType, const uint32_t sampleRate)
        : _format(format),
          _payloadType(payloadType),
          _sampleRate(sampleRate)
    {
    }

    RtpMap(const Format format,
        const uint32_t payloadType,
        const uint32_t sampleRate,
        const utils::Optional<uint32_t>& channels)
        : _format(format),
          _payloadType(payloadType),
          _sampleRate(sampleRate),
          _channels(channels)
    {
    }

    RtpMap(const RtpMap& rtpMap) = default;

    Format _format;
    uint32_t _payloadType;
    uint32_t _sampleRate;
    utils::Optional<uint32_t> _channels;
    std::unordered_map<std::string, std::string> _parameters;
    std::vector<std::pair<std::string, utils::Optional<std::string>>> _rtcpFeedbacks;
};

} // namespace bridge
