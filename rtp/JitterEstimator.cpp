#include "rtp/JitterEstimator.h"
#include "logger/Logger.h"
#include "utils/Time.h"
#include <array>
#include <cstdint>

#define JITTER_DEBUG 0

#if JITTER_DEBUG
#define LOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
#define LOG(fmt, ...)
#endif

namespace
{

} // namespace

namespace rtp
{
JitterEstimator::JitterEstimator(uint32_t sampleFrequency)
    : _var(150),
      _maxJitter(0.03),
      _maxJitterStable(0.003),
      _delayTracker(sampleFrequency)
{
}

/**
 * returns delay of received packet in ms
 */
double JitterEstimator::update(uint64_t receiveTime, uint32_t rtpTimestamp)
{
    auto rawDelay = _delayTracker.update(receiveTime, rtpTimestamp);

    const double measurement = static_cast<double>(rawDelay / utils::Time::us) / 1000.0;

    _var.add(measurement);
    _maxJitter.update(measurement);
    _maxJitterStable.update(measurement);
    if (_maxJitter.get() * 2 < _maxJitterStable.get() && _maxJitter.get() > 30.0)
    {
        logger::info("Jitter estimate reset %.2f -> %.2f", "JitterEstimator", _maxJitterStable.get(), _maxJitter.get());
        _maxJitterStable.reset(_maxJitter.get());
    }

    LOG("%.2fs jitter %.4f, d %.2fms, std %.4f, 95p %.4f, maxJ %.4f, Jslow %.3f ",
        "JitterEstimator",
        rtpTimestamp / 48000.0,
        _var.getMean(),
        measurement,
        sqrt(_var.getVariance()),
        get95Percentile(),
        _maxJitter.get(),
        _maxJitterStable.get());

    return measurement;
}

/**
 * avg jitter in ms
 */
double JitterEstimator::getJitter() const
{
    return _var.getMean();
}

double JitterEstimator::get95Percentile() const
{
    return _var.getMean() + 2 * sqrt(_var.getVariance());
}

} // namespace rtp
