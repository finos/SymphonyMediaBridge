#pragma once

#include "concurrency/MpmcHashmap.h"
#include "logger/Logger.h"
#include "memory/List.h"
#include "utils/ScopedReentrancyBlocker.h"
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

    explicit VideoMissingPacketsTracker(const uint64_t intervalMs)
        : _loggableId("VideoMissingPacketsTracker"),
          _intervalMs(intervalMs),
#if DEBUG
          _producerCounter(0),
          _consumerCounter(0),
#endif
          _missingPackets(maxMissingPackets * 2),
          _lastRunTimestampMs(0),
          _resetTimestampMs(0)
    {
    }

    void onMissingPacket(const uint32_t extendedSequenceNumber, const uint64_t timestampMs)
    {
        REENTRANCE_CHECK(_producerCounter);

        if (_missingPackets.size() >= maxMissingPackets)
        {
            TRACKER_LOG("Too many missing packets", _loggableId.c_str());
            _resetTimestampMs = timestampMs;
            return;
        }

        TRACKER_LOG("Add missing packet seq %u", _loggableId.c_str(), extendedSequenceNumber);

        _missingPackets.emplace(extendedSequenceNumber & 0xFFFF,
            Entry({timestampMs, 0, 0, extendedSequenceNumber, false}));
        return;
    }

    bool onPacketArrived(const uint16_t sequenceNumber, uint32_t& outExtendedSequenceNumber)
    {
        REENTRANCE_CHECK(_producerCounter);

        auto missingPacketsItr = _missingPackets.find(sequenceNumber);
        if (missingPacketsItr == _missingPackets.end())
        {
            return false;
        }
        else if (missingPacketsItr->second._arrived)
        {
            TRACKER_LOG("Late packet arrived seq %u already removed", _loggableId.c_str(), sequenceNumber);
            return false;
        }

        missingPacketsItr->second._arrived = true;
        outExtendedSequenceNumber = missingPacketsItr->second._extendedSequenceNumber;
        TRACKER_LOG("Late packet arrived seq %u (seq %u roc %u)",
            _loggableId.c_str(),
            sequenceNumber,
            outExtendedSequenceNumber & 0xFFFF,
            outExtendedSequenceNumber >> 16);

        return true;
    }

    void reset(const uint64_t timestampMs)
    {
        REENTRANCE_CHECK(_producerCounter);
        _resetTimestampMs = timestampMs;
    }

    bool shouldProcess(const uint64_t timestampMs) const { return (timestampMs - _lastRunTimestampMs) >= _intervalMs; }

    size_t process(const uint64_t timestampMs,
        const uint32_t rttMs,
        std::array<uint16_t, maxMissingPackets>& outMissingSequenceNumbers)
    {
        REENTRANCE_CHECK(_consumerCounter);

        size_t returnSize = 0;

        std::array<uint16_t, maxMissingPackets * 2> entriesToErase;
        size_t numEntriesToErase = 0;
        // Add 10 ms, since incoming packets are processed with 10 ms intervals in EngineMixer
        const uint64_t initialDelayMs = rttMs + 10;
        const uint64_t minDelayMs = 100;
        const uint64_t retryDelay = std::max(initialDelayMs, minDelayMs);

        for (auto& missingPacketEntry : _missingPackets)
        {
            if (missingPacketEntry.second._timestampMs <= _resetTimestampMs)
            {
                TRACKER_LOG("No more nack for seq %u, older than reset time",
                    _loggableId.c_str(),
                    missingPacketEntry.first);

                assert(numEntriesToErase < entriesToErase.size());
                entriesToErase[numEntriesToErase] = missingPacketEntry.first;
                ++numEntriesToErase;
                continue;
            }

            if (missingPacketEntry.second._arrived)
            {
                assert(numEntriesToErase < entriesToErase.size());
                entriesToErase[numEntriesToErase] = missingPacketEntry.first;
                ++numEntriesToErase;
                continue;
            }

            if ((timestampMs - missingPacketEntry.second._timestampMs > initialDelayMs) &&
                (timestampMs - missingPacketEntry.second._lastSentNackTimestampMs > retryDelay))
            {
                if (missingPacketEntry.second._nacksSent >= maxRetries)
                {
                    TRACKER_LOG("No more nack for seq %u, max retries hit",
                        _loggableId.c_str(),
                        missingPacketEntry.first);

                    assert(numEntriesToErase < entriesToErase.size());
                    entriesToErase[numEntriesToErase] = missingPacketEntry.first;
                    ++numEntriesToErase;
                    continue;
                }

                missingPacketEntry.second._lastSentNackTimestampMs = timestampMs;
                outMissingSequenceNumbers[returnSize] = missingPacketEntry.first;
                ++returnSize;
                ++missingPacketEntry.second._nacksSent;

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
        uint64_t _timestampMs;
        uint64_t _lastSentNackTimestampMs;
        uint32_t _nacksSent;
        uint32_t _extendedSequenceNumber;
        bool _arrived;
    };

    logger::LoggableId _loggableId;
    uint64_t _intervalMs;
#if DEBUG
    std::atomic_uint32_t _producerCounter;
    std::atomic_uint32_t _consumerCounter;
#endif
    concurrency::MpmcHashmap32<uint16_t, Entry> _missingPackets;
    uint64_t _lastRunTimestampMs;
    uint64_t _resetTimestampMs;
};

} // namespace bridge
