
#pragma once
#include "test/bwe/FakeMedia.h"
#include "utils/Time.h"
#include "utils/Trackers.h"
#include <cassert>
#include <cstdint>
#include <gtest/gtest.h>
#include <inttypes.h>
#include <memory>
#include <queue>
#include <random>
#include <thread>
#include <unistd.h>

namespace memory
{
class Packet;
}
namespace fakenet
{
class NetworkLink
{
public:
    explicit NetworkLink(uint32_t bandwidthKbps, size_t bufferSize, size_t mtu)
        : _releaseTime(0),
          _bandwidthKbps(bandwidthKbps),
          _mtu(mtu),
          _queuedBytes(0),
          _bufferCapacity(bufferSize),
          _lossRate(0),
          _burstIntervalUs(0),
          _bitRate(200 * utils::Time::ms)
    {
    }

    bool push(memory::Packet* packet, uint64_t timestamp);
    memory::Packet* pop(uint64_t timestamp);
    memory::Packet* pop();
    size_t count() const { return _queue.size(); }
    bool empty() const { return _queue.empty(); }
    void setMTU(size_t mtu) { _mtu = mtu; }
    size_t getSctpMTU() const { return _mtu - IPOVERHEAD; }

    int64_t timeToRelease(uint64_t timestamp) const;

    void setLossRate(double rate) { _lossRate = std::max(0.0, std::min(1.0, rate)); }
    void setBurstDeliveryInterval(uint32_t ms);
    void injectDelaySpike(uint32_t ms);

    double getBitRateKbps(uint64_t timestamp) const
    {
        return _bitRate.get(timestamp, 1070 * utils::Time::ms) * utils::Time::sec / 1000;
    }

    size_t getQueueLength() const { return _queuedBytes; }

    uint32_t getBandwidthKbps() const { return _bandwidthKbps; }
    void setBandwidthKbps(uint32_t bandwidth) { _bandwidthKbps = bandwidth; }

    static const int IPOVERHEAD = 20 + 13; // IP and DTLS header
private:
    void addBurstDelay();

    std::queue<memory::Packet*> _queue;
    uint64_t _releaseTime;
    uint32_t _bandwidthKbps;

    size_t _mtu;
    size_t _queuedBytes;
    const size_t _bufferCapacity;
    double _lossRate;
    uint64_t _burstIntervalUs;
    utils::RateTracker<10> _bitRate;
};

} // namespace fakenet