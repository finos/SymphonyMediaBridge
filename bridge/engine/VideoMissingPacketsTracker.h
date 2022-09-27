#pragma once

#include "concurrency/MpmcHashmap.h"
#include "logger/Logger.h"
#include "memory/List.h"
#include "utils/ScopedReentrancyBlocker.h"
#include "utils/Time.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

#define DEBUG_MISSING_PACKETS_TRACKER 0

#if DEBUG_MISSING_PACKETS_TRACKER
#define TRACKER_LOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
#define TRACKER_LOG(fmt, ...)
#endif

namespace bridge
{

class VideoMissingPacketsTracker
{
public:
    static const size_t maxMissingPackets = 128;

    explicit VideoMissingPacketsTracker()
        : _loggableId("VideoMissingPacketsTracker"),
#if DEBUG
          _producerCounter(0),
          _consumerCounter(0),
#endif
          _missingPackets(maxMissingPackets * 2),
          _resetTimestamp(0)
    {
    }

    void onMissingPacket(const uint32_t extendedSequenceNumber, const uint64_t timestamp)
    {
        REENTRANCE_CHECK(_producerCounter);

        if (_missingPackets.size() >= maxMissingPackets)
        {
            TRACKER_LOG("Too many missing packets", _loggableId.c_str());
            _resetTimestamp = timestamp;
            return;
        }

        TRACKER_LOG("Add missing packet seq %u", _loggableId.c_str(), extendedSequenceNumber);

        _missingPackets.emplace(extendedSequenceNumber & 0xFFFF,
            Entry({timestamp, 0, 0, extendedSequenceNumber, false}));
        return;
    }

    bool onPacketArrived(const uint16_t sequenceNumber, uint32_t& outExtendedSequenceNumber)
    {
        REENTRANCE_CHECK(_producerCounter);

        auto* missingPacket = _missingPackets.getItem(sequenceNumber);
        if (!missingPacket)
        {
            return false;
        }

        if (missingPacket->arrived)
        {
            TRACKER_LOG("Late packet arrived seq %u already removed", _loggableId.c_str(), sequenceNumber);
            return false;
        }

        missingPacket->arrived = true;
        outExtendedSequenceNumber = missingPacket->extendedSequenceNumber;
        TRACKER_LOG("Late packet arrived seq %u (seq %u roc %u)",
            _loggableId.c_str(),
            sequenceNumber,
            outExtendedSequenceNumber & 0xFFFF,
            outExtendedSequenceNumber >> 16);

        return true;
    }

    void reset(const uint64_t timestamp)
    {
        REENTRANCE_CHECK(_producerCounter);
        _resetTimestamp = timestamp;
    }

    size_t process(const uint64_t timestamp,
        const uint32_t rttNs,
        std::array<uint16_t, maxMissingPackets>& outMissingSequenceNumbers)
    {
        REENTRANCE_CHECK(_consumerCounter);

        size_t returnSize = 0;

        std::array<uint16_t, maxMissingPackets * 2> entriesToErase;
        size_t numEntriesToErase = 0;
        // Add 10 ms, since incoming packets are processed with 10 ms intervals in EngineMixer
        const uint64_t initialDelay = rttNs + 10 * utils::Time::ms;
        const uint64_t minDelay = 100 * utils::Time::ms;
        const uint64_t retryDelay = std::max(initialDelay, minDelay);

        for (auto& missingPacketEntry : _missingPackets)
        {
            if (missingPacketEntry.second.timestamp <= _resetTimestamp)
            {
                TRACKER_LOG("No more nack for seq %u, older than reset time",
                    _loggableId.c_str(),
                    missingPacketEntry.first);

                assert(numEntriesToErase < entriesToErase.size());
                entriesToErase[numEntriesToErase] = missingPacketEntry.first;
                ++numEntriesToErase;
                continue;
            }

            if (missingPacketEntry.second.arrived)
            {
                assert(numEntriesToErase < entriesToErase.size());
                entriesToErase[numEntriesToErase] = missingPacketEntry.first;
                ++numEntriesToErase;
                continue;
            }

            if ((timestamp - missingPacketEntry.second.timestamp > initialDelay) &&
                (timestamp - missingPacketEntry.second.lastSentNackTimestamp > retryDelay))
            {
                if (missingPacketEntry.second.nacksSent >= maxRetries)
                {
                    TRACKER_LOG("No more nack for seq %u, max retries hit",
                        _loggableId.c_str(),
                        missingPacketEntry.first);

                    assert(numEntriesToErase < entriesToErase.size());
                    entriesToErase[numEntriesToErase] = missingPacketEntry.first;
                    ++numEntriesToErase;
                    continue;
                }

                missingPacketEntry.second.lastSentNackTimestamp = timestamp;
                outMissingSequenceNumbers[returnSize] = missingPacketEntry.first;
                ++returnSize;
                ++missingPacketEntry.second.nacksSent;

                if (returnSize >= maxMissingPackets)
                {
                    break;
                }
            }
        }

        for (size_t i = 0; i < numEntriesToErase; ++i)
        {
            _missingPackets.erase(entriesToErase[i]);
        }

        return returnSize;
    }

private:
    static const uint32_t maxRetries = 4;

    struct Entry
    {
        uint64_t timestamp = 0;
        uint64_t lastSentNackTimestamp = 0;
        uint32_t nacksSent = 0;
        uint32_t extendedSequenceNumber = 0;
        bool arrived = false;
    };

    logger::LoggableId _loggableId;
#if DEBUG
    std::atomic_uint32_t _producerCounter;
    std::atomic_uint32_t _consumerCounter;
#endif
    concurrency::MpmcHashmap32<uint16_t, Entry> _missingPackets;
    uint64_t _resetTimestamp;
};

} // namespace bridge
