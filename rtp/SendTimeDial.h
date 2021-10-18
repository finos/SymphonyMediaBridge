#pragma once
#include <cstdint>

namespace rtp
{

// Tracks and converts abs send time from a time line with resolution 3.8us
// and a time line with 1 ns resolution
// It must handle
// - reordering of packets
// - longer receive paus
// - maintain precision over long period of time
class SendTimeDial
{
public:
    SendTimeDial();

    uint64_t toAbsoluteTime(uint32_t sendTime6_18, uint64_t timestamp);

private:
    uint64_t _timeReference;
    uint32_t _sendTimeReference;
    bool _initialized;
};
} // namespace rtp