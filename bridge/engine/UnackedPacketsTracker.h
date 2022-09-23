#pragma once

#include "concurrency/MpmcHashmap.h"
#include "logger/Logger.h"
#include "memory/List.h"
#include "utils/ScopedReentrancyBlocker.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

#define DEBUG_UNACKED_PACKETS_TRACKER 0

#if DEBUG_UNACKED_PACKETS_TRACKER
#define UNACK_LOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
#define UNACK_LOG(fmt, ...)
#endif

namespace bridge
{

class UnackedPacketsTracker
{
public:
    static const size_t maxUnackedPackets = 64;
    static const uint64_t initialDelay = 50;
    static const uint64_t retryDelay = 50;

    explicit UnackedPacketsTracker(const char* name, const uint64_t intervalMs)
        : _loggableId(name),
          _intervalMs(intervalMs),
#if DEBUG
          _producerCounter(0),
          _consumerCounter(0),
#endif
          _unackedPackets(maxUnackedPackets * 2),
          _lastRunTimestampMs(0),
          _resetTimestampMs(0)
    {
    }

    void onPacketSent(const uint32_t extendedSequenceNumber, const uint64_t timestampMs)
    {
        REENTRANCE_CHECK(_producerCounter);

        if (_unackedPackets.size() >= maxUnackedPackets)
        {
            UNACK_LOG("Too many unacked packets", _loggableId.c_str());
            _resetTimestampMs = timestampMs;
            return;
        }

        UNACK_LOG("Add unacked packet seq %u", _loggableId.c_str(), extendedSequenceNumber);
        _unackedPackets.emplace(extendedSequenceNumber & 0xFFFF,
            Entry({timestampMs, timestampMs, 0, extendedSequenceNumber, false}));
    }

    bool onPacketAcked(const uint16_t sequenceNumber, uint32_t& outExtendedSequenceNumber)
    {
        REENTRANCE_CHECK(_producerCounter);

        auto missingPacketsItr = _unackedPackets.find(sequenceNumber);
        if (missingPacketsItr == _unackedPackets.end() || missingPacketsItr->second._acked)
        {
            UNACK_LOG("Late packet ack seq %u already removed", _loggableId.c_str(), sequenceNumber);
            return false;
        }

        missingPacketsItr->second._acked = true;
        outExtendedSequenceNumber = missingPacketsItr->second._extendedSequenceNumber;

        UNACK_LOG("Late ack arrived seq %u (seq %u roc %u)",
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

    size_t process(const uint64_t timestampMs, std::array<uint16_t, maxUnackedPackets>& outMissingSequenceNumbers)
    {
        REENTRANCE_CHECK(_consumerCounter);

        size_t returnSize = 0;

        std::array<uint16_t, maxUnackedPackets * 2> entriesToErase{};
        size_t numEntriesToErase = 0;

        for (auto& unackedPacketEntry : _unackedPackets)
        {
            if (unackedPacketEntry.second._timestampMs <= _resetTimestampMs)
            {
                UNACK_LOG("No more sends for seq %u, older than reset time",
                    _loggableId.c_str(),
                    unackedPacketEntry.first);

                assert(numEntriesToErase < entriesToErase.size());
                entriesToErase[numEntriesToErase] = unackedPacketEntry.first;
                ++numEntriesToErase;
                continue;
            }

            if (unackedPacketEntry.second._acked)
            {
                assert(numEntriesToErase < entriesToErase.size());
                entriesToErase[numEntriesToErase] = unackedPacketEntry.first;
                ++numEntriesToErase;
                continue;
            }

            if ((timestampMs - unackedPacketEntry.second._timestampMs > initialDelay) &&
                (timestampMs - unackedPacketEntry.second._lastSentTimestampMs > retryDelay))
            {
                if (unackedPacketEntry.second._sentCount >= maxRetries)
                {
                    UNACK_LOG("No more sends for seq %u, max retries hit",
                        _loggableId.c_str(),
                        unackedPacketEntry.first);

                    assert(numEntriesToErase < entriesToErase.size());
                    entriesToErase[numEntriesToErase] = unackedPacketEntry.first;
                    ++numEntriesToErase;
                    continue;
                }

                unackedPacketEntry.second._lastSentTimestampMs = timestampMs;
                outMissingSequenceNumbers[returnSize] = unackedPacketEntry.first;
                ++returnSize;
                ++unackedPacketEntry.second._sentCount;

                if (returnSize >= maxUnackedPackets)
                {
                    break;
                }
            }
        }

        for (size_t i = 0; i < numEntriesToErase; ++i)
        {
            _unackedPackets.erase(entriesToErase[i]);
        }

        return returnSize;
    }

private:
    static const uint32_t maxRetries = 4;

    struct Entry
    {
        uint64_t _timestampMs;
        uint64_t _lastSentTimestampMs;
        uint32_t _sentCount;
        uint32_t _extendedSequenceNumber;
        bool _acked;
    };

    logger::LoggableId _loggableId;
    uint64_t _intervalMs;
#if DEBUG
    std::atomic_uint32_t _producerCounter;
    std::atomic_uint32_t _consumerCounter;
#endif
    concurrency::MpmcHashmap32<uint16_t, Entry> _unackedPackets;
    uint64_t _lastRunTimestampMs;
    uint64_t _resetTimestampMs;
};

} // namespace bridge
