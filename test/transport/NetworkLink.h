
#pragma once
#include "memory/PacketPoolAllocator.h"
#include "utils/Time.h"
#include "utils/Trackers.h"
#include <gtest/gtest.h>
#include <inttypes.h>
#include <mutex>
#include <queue>
#include <string>
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
    explicit NetworkLink(std::string name, uint32_t bandwidthKbps, size_t bufferSize, size_t mtu)
        : _name(name),
          _releaseTime(0),
          _bandwidthKbps(bandwidthKbps),
          _staticDelay(0),
          _mtu(mtu),
          _queuedBytes(0),
          _bufferCapacity(bufferSize),
          _lossRate(0),
          _burstIntervalUs(0),
          _bitRate(200 * utils::Time::ms)
    {
    }

    bool push(memory::UniquePacket packet, uint64_t timestamp);
    memory::UniquePacket pop(uint64_t timestamp);
    memory::UniquePacket pop();
    size_t count() const { return _queue.size() + _delayQueue.size(); }
    bool empty() const { return _queue.empty() && _delayQueue.empty(); }
    void setMTU(size_t mtu) { _mtu = mtu; }
    size_t getSctpMTU() const { return _mtu - IPOVERHEAD; }

    int64_t timeToRelease(uint64_t timestamp) const;

    void setLossRate(double rate) { _lossRate = std::max(0.0, std::min(1.0, rate)); }
    void setBurstDeliveryInterval(uint32_t ms);
    void injectDelaySpike(uint32_t ms);
    void setStaticDelay(uint32_t ms);

    double getBitRateKbps(uint64_t timestamp) const
    {
        return _bitRate.get(timestamp, 1070 * utils::Time::ms) * utils::Time::sec / 1000;
    }

    size_t getQueueLength() const { return _queuedBytes; }

    uint32_t getBandwidthKbps() const { return _bandwidthKbps; }
    void setBandwidthKbps(uint32_t bandwidth) { _bandwidthKbps = bandwidth; }

    std::string getName() const { return _name; }

    static const int IPOVERHEAD = 20 + 14; // IP and DTLS header
private:
    void addBurstDelay();
    memory::UniquePacket popDelayQueue(uint64_t timestamp);

    std::string _name;
    std::queue<memory::UniquePacket> _queue;
    uint64_t _releaseTime;
    uint32_t _bandwidthKbps;

    struct DelayEntry
    {
        DelayEntry(memory::UniquePacket packetPtr, uint64_t relTime)
            : packet(std::move(packetPtr)),
              releaseTime(relTime)
        {
        }

        memory::UniquePacket packet;
        uint64_t releaseTime = 0;
    };
    std::queue<DelayEntry> _delayQueue;
    uint64_t _staticDelay;

    size_t _mtu;
    size_t _queuedBytes;
    const size_t _bufferCapacity;
    double _lossRate;
    uint64_t _burstIntervalUs;
    utils::RateTracker<10> _bitRate;
    std::mutex _pushMutex;
};

} // namespace fakenet
