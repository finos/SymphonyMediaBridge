#pragma once
#include <cstdint>

namespace bwe
{

class Estimator
{
public:
    virtual ~Estimator() {}
    virtual void update(uint32_t packetSize, uint64_t transmitTimeNs, uint64_t receiveTimeNs) = 0;
    virtual void onUnmarkedTraffic(uint32_t packetSize, uint64_t receiveTimeNs) = 0;

    virtual double getEstimate(uint64_t timestamp) const = 0;
    virtual double getDelay() const = 0;
    virtual double getReceiveRate(uint64_t timestamp) const = 0;
};
} // namespace bwe