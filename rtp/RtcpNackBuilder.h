#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rtp
{

struct RtcpFeedback;

class RtcpNackBuilder
{
public:
    static const size_t maxPids = 16;

    RtcpNackBuilder(const uint32_t reporterSsrc, const uint32_t mediaSsrc);

    bool appendSequenceNumber(const uint16_t sequenceNumber);
    const uint8_t* build(size_t& outSize);

private:
    static const size_t dataSize = 128;

    std::array<uint8_t, dataSize> _data;
    RtcpFeedback* _rtcpFeedback;
    std::array<uint16_t, maxPids> _pids;
    std::array<uint16_t, maxPids> _blps;
    size_t _nextFreePid;
};

} // namespace rtp
