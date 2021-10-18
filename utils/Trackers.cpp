#include "Trackers.h"
#include "logger/Logger.h"
namespace utils
{

void AvgTracker::update(double value)
{
    while (true)
    {
        auto currentValue = _value.load();
        if (_value.compare_exchange_strong(currentValue, currentValue + _alpha * (value - currentValue)))
        {
            break;
        }
    }
}

double AvgRateTracker::get() const
{
    if (_avgTime == 0)
    {
        return 0;
    }
    return _value * utils::Time::sec / _avgTime;
}

void AvgRateTracker::update(double value, uint64_t timestamp)
{
    if (_prevTimestamp == 0)
    {
        _prevTimestamp = timestamp - utils::Time::ms * 5;
    }

    _value += _alpha * (value - _value);
    _avgTime += _alpha * ((timestamp - _prevTimestamp) - _avgTime);
    _prevTimestamp = timestamp;
}

void MaxTracker::update(double value)
{
    while (true)
    {
        auto currentValue = _value.load();
        if (currentValue <= value && _value.compare_exchange_strong(currentValue, value))
        {
            break;
        }
        else if (currentValue > value &&
            _value.compare_exchange_strong(currentValue, currentValue - _decay * (currentValue - value)))
        {
            break;
        }
    }
}

void TimeTracker::stop()
{
    _stopTime = Time::getApproximateTime();
}

uint64_t TimeTracker::getElapsed() const
{
    if (static_cast<int64_t>(_start - _stopTime) > 0)
    {
        return Time::getApproximateTime() - _start;
    }
    return _stopTime - _start;
}

void TimeGuard::check()
{
    auto elapsed = _timeTracker.getElapsed();
    if (elapsed > _maxTime)
    {
        logger::debug("overshoot %s%d took %" PRId64 "us expected %" PRId64 "us\n",
            _fileName,
            _name,
            _lineNumber,
            elapsed / 1000,
            _maxTime / 1000);
    }
    _maxTime = 1000000000 + _lineNumber; // no fire again
}

} // namespace utils