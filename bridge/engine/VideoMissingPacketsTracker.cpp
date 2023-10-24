#include "bridge/engine/VideoMissingPacketsTracker.h"
#include "utils/ScopedReentrancyBlocker.h"
#include "utils/Time.h"
/*#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
*/

#define DEBUG_MISSING_PACKETS_TRACKER 0

#if DEBUG_MISSING_PACKETS_TRACKER
#define TRACKER_LOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
#define TRACKER_LOG(fmt, ...)
#endif

namespace bridge
{
const uint64_t delayFirstNack = 30 * utils::Time::ms;

VideoMissingPacketsTracker::VideoMissingPacketsTracker()
    : _loggableId("VideoMissingPacketsTracker"),
#if DEBUG
      _producerCounter(0),
#endif
      _missingPackets(maxMissingPackets)
{
}

bool VideoMissingPacketsTracker::onMissingPacket(const uint32_t extendedSequenceNumber, const uint64_t timestamp)
{
    REENTRANCE_CHECK(_producerCounter);

    if (_missingPackets.size() >= maxMissingPackets)
    {
        logger::info("Too many missing packets", _loggableId.c_str());
        reset();
        return false;
    }

    TRACKER_LOG("Add missing packet seq %u", _loggableId.c_str(), extendedSequenceNumber);

    _missingPackets.emplace(extendedSequenceNumber & 0xFFFF,
        Entry({timestamp, timestamp - 5 * utils::Time::sec, 0, extendedSequenceNumber}));
    return true;
}

bool VideoMissingPacketsTracker::onPacketArrived(const uint16_t sequenceNumber)
{
    uint32_t dummy;
    return onPacketArrived(sequenceNumber, dummy);
}

bool VideoMissingPacketsTracker::onPacketArrived(const uint16_t sequenceNumber, uint32_t& outExtendedSequenceNumber)
{
    REENTRANCE_CHECK(_producerCounter);

    const auto* missingPacket = _missingPackets.getItem(sequenceNumber);
    if (!missingPacket)
    {
        return false;
    }

    outExtendedSequenceNumber = missingPacket->extendedSequenceNumber;
    _missingPackets.erase(sequenceNumber);

    TRACKER_LOG("Late packet arrived seq %u (seq %u roc %u)",
        _loggableId.c_str(),
        sequenceNumber,
        outExtendedSequenceNumber & 0xFFFF,
        outExtendedSequenceNumber >> 16);

    return true;
}

void VideoMissingPacketsTracker::reset()
{
    REENTRANCE_CHECK(_producerCounter);
    _missingPackets.clear();
}

size_t VideoMissingPacketsTracker::process(const uint64_t timestamp,
    const uint32_t rttNs,
    std::array<uint16_t, maxMissingPackets>& outMissingSequenceNumbers)
{
    REENTRANCE_CHECK(_producerCounter);

    size_t missingCount = 0;

    // Add 10 ms, since incoming packets are processed with 10 ms intervals in EngineMixer
    const uint64_t retryDelay = std::max(rttNs + 10 * utils::Time::ms, 100 * utils::Time::ms);

    for (auto& entryIt : _missingPackets)
    {
        auto& entry = entryIt.second;

        if (utils::Time::diffGT(entry.timestamp, timestamp, delayFirstNack) &&
            utils::Time::diffGT(entry.lastSentNackTimestamp, timestamp, retryDelay))
        {
            if (++entry.nacksSent > maxRetries)
            {
                _missingPackets.erase(entryIt.first);
                continue;
            }

            entry.lastSentNackTimestamp = timestamp;
            outMissingSequenceNumbers[missingCount] = entryIt.first;
            ++missingCount;

            assert(missingCount < maxMissingPackets);
            if (missingCount >= maxMissingPackets)
            {
                break;
            }
        }
    }

    return missingCount;
}

} // namespace bridge
