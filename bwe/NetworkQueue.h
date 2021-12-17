#pragma once
#include "utils/Time.h"
#include <algorithm>
#include <cstdint>

namespace bwe
{

/** Models a network FIFO queue that drains at a specific bit rate.
 */
class NetworkQueue
{
public:
    NetworkQueue() : _queue(0), _lastTransmission(0), _bandwidthKbps(200) {}
    explicit NetworkQueue(uint32_t kbps) : _queue(0), _lastTransmission(0), _bandwidthKbps(kbps) {}

    void setBandwidth(uint32_t kbps) { _bandwidthKbps = kbps; }
    uint32_t getBandwidth() const { return _bandwidthKbps; }

    void onPacketSent(uint64_t timestamp, uint16_t size)
    {
        _queue -= std::min(_queue,
            static_cast<uint32_t>(
                utils::Time::diff(_lastTransmission, timestamp) * _bandwidthKbps / (8 * utils::Time::ms)));

        _queue += size;
        _lastTransmission = timestamp;
    }

    void drain(uint64_t timestamp) { onPacketSent(timestamp, 0); }
    void clear() { _queue = 0; }
    void setSize(uint32_t bytes) { _queue = bytes; }

    uint32_t size() const { return _queue; }
    uint32_t predictQueueAt(uint64_t timestamp) const
    {
        return _queue -
            std::min(_queue,
                static_cast<uint32_t>(
                    utils::Time::diff(_lastTransmission, timestamp) * _bandwidthKbps / (8 * utils::Time::ms)));
    }

private:
    uint32_t _queue;
    uint64_t _lastTransmission;
    uint32_t _bandwidthKbps;
};
} // namespace bwe
