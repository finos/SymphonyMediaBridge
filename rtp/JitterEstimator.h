#pragma once
#include "math/Matrix.h"
#include "math/WelfordVariance.h"
#include "rtp/RtpDelayTracker.h"
#include "utils/Trackers.h"

namespace rtp
{

template <typename T, size_t S>
class Backlog
{
public:
    T add(T value)
    {
        _index = (_index + 1) % S;
        auto prev = _values[_index];
        _values[_index] = value;
        return prev;
    }

    T front() { return _values[_index]; }
    T back() { return _values[(_index + 1) % S]; }

    T getMean() const
    {
        T acc = 0;
        for (int i = 0; i < S; ++i)
        {
            acc += _values[i];
        }

        return acc / S;
    }

    T getVariance(T hypotheticalMean) const
    {
        T acc = 0;
        for (size_t i = 0; i < S; ++i)
        {
            auto d = _values[i] - hypotheticalMean;
            acc += d * d;
        }

        return acc / S;
    }

private:
    T _values[S];
    uint32_t _index;
};
/**
 * Estimates the latency from jitter. That is the jitter buffer level you will need to be able to replay
 * media without having gaps due to jitter. The 95percentile level will calculate level needed to replay >95%
 * of the packets without gap.
 * This is far from the jitter tracker in RFC3550 that average over delta in RTP timestamp vs delta in receive time.
 */
class JitterEstimator
{
public:
    JitterEstimator(uint32_t sampleFrequency);

    double update(uint64_t receiveTime, uint32_t rtpTimestamp);

    double getJitter() const;
    double get95Percentile() const;
    double getMaxJitter() const { return _maxJitter.get(); }
    double getJitterMaxStable() const { return _maxJitterStable.get(); }
    uint32_t toRtpTimestamp(uint64_t timestamp) const { return _delayTracker.toRtpTimestamp(timestamp); }
    uint32_t getRtpFrequency() const { return _delayTracker.getFrequency(); }

private:
    void updateVarianceAccumulator(double value);

    math::RollingWelfordVariance<double> _var;

    utils::MaxTrackerPlain _maxJitter;
    utils::MaxTrackerPlain _maxJitterStable;

    RtpDelayTracker _delayTracker;
};

} // namespace rtp
