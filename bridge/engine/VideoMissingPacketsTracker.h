#pragma once

#include "concurrency/MpmcHashmap.h"
#include "logger/Logger.h"
#include <array>
#include <atomic>

namespace bridge
{

class VideoMissingPacketsTracker
{
public:
    static const size_t maxMissingPackets = 128;

    explicit VideoMissingPacketsTracker();

    bool onMissingPacket(const uint32_t extendedSequenceNumber, const uint64_t timestamp);
    bool onPacketArrived(const uint16_t sequenceNumber, uint32_t& outExtendedSequenceNumber);
    bool onPacketArrived(const uint16_t sequenceNumber);

    void reset();

    size_t process(const uint64_t timestamp,
        const uint32_t rttNs,
        std::array<uint16_t, maxMissingPackets>& outMissingSequenceNumbers);

private:
    static const uint32_t maxRetries = 4;

    struct Entry
    {
        const uint64_t timestamp = 0;
        uint64_t lastSentNackTimestamp = 0;
        uint32_t nacksSent = 0;
        const uint32_t extendedSequenceNumber = 0;
    };

    logger::LoggableId _loggableId;
#if DEBUG
    std::atomic_uint32_t _producerCounter;
#endif
    concurrency::MpmcHashmap32<uint16_t, Entry> _missingPackets;
};

} // namespace bridge
