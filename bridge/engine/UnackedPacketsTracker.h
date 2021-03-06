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
#if DEBUG
        utils::ScopedReentrancyBlocker blocker(_producerCounter);
#endif

        if (_unackedPackets.size() >= maxUnackedPackets)
        {
#if DEBUG_UNACKED_PACKETS_TRACKER
            logger::debug("Too many unacked packets", _loggableId.c_str());
#endif
            _resetTimestampMs = timestampMs;
            return;
        }

#if DEBUG_UNACKED_PACKETS_TRACKER
        logger::debug("Add unacked packet seq %u", _loggableId.c_str(), extendedSequenceNumber);
#endif
        _unackedPackets.emplace(extendedSequenceNumber & 0xFFFF,
            Entry({timestampMs, timestampMs, 0, extendedSequenceNumber, false}));
    }

    bool onPacketAcked(const uint16_t sequenceNumber, uint32_t& outExtendedSequenceNumber)
    {
#if DEBUG
        utils::ScopedReentrancyBlocker blocker(_producerCounter);
#endif

        auto missingPacketsItr = _unackedPackets.find(sequenceNumber);
        if (missingPacketsItr == _unackedPackets.end() || missingPacketsItr->second._acked)
        {
#if DEBUG_UNACKED_PACKETS_TRACKER
            logger::debug("Late packet ack seq %u already removed", _loggableId.c_str(), sequenceNumber);
#endif
            return false;
        }

        missingPacketsItr->second._acked = true;
        outExtendedSequenceNumber = missingPacketsItr->second._extendedSequenceNumber;
#if DEBUG_UNACKED_PACKETS_TRACKER
        logger::debug("Late ack arrived seq %u (seq %u roc %u)",
            _loggableId.c_str(),
            sequenceNumber,
            outExtendedSequenceNumber & 0xFFFF,
            outExtendedSequenceNumber >> 16);
#endif
        return true;
    }

    void reset(const uint64_t timestampMs)
    {
#if DEBUG
        utils::ScopedReentrancyBlocker blocker(_producerCounter);
#endif
        _resetTimestampMs = timestampMs;
    }

    bool shouldProcess(const uint64_t timestampMs) const { return (timestampMs - _lastRunTimestampMs) >= _intervalMs; }

    size_t process(const uint64_t timestampMs, std::array<uint16_t, maxUnackedPackets>& outMissingSequenceNumbers)
    {
#if DEBUG
        utils::ScopedReentrancyBlocker blocker(_consumerCounter);
#endif

        size_t returnSize = 0;

        std::array<uint16_t, maxUnackedPackets * 2> entriesToErase{};
        size_t numEntriesToErase = 0;

        for (auto& unackedPacketEntry : _unackedPackets)
        {
            if (unackedPacketEntry.second._timestampMs <= _resetTimestampMs)
            {
#if DEBUG_UNACKED_PACKETS_TRACKER
                logger::debug("No more sends for seq %u, older than reset time",
                    _loggableId.c_str(),
                    unackedPacketEntry.first);
#endif
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
#if DEBUG_UNACKED_PACKETS_TRACKER
                    logger::debug("No more sends for seq %u, max retries hit",
                        _loggableId.c_str(),
                        unackedPacketEntry.first);
#endif

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
