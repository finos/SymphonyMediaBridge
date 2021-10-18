#include "BwBurstTracker.h"

namespace bwe
{

BwBurstTracker::BwBurstTracker()
    : _head(0),
      _tail(0),
      _bucketTotal(0),
      _previousReport(LIMIT_FIRST_BUCKET),
      _previousReceiveTime(0)
{
    for (auto& b : _buckets)
    {
        b = 0;
    }
}

void BwBurstTracker::onPacketReceived(uint32_t packetSize, uint64_t receiveTime)
{
    if (_previousReceiveTime == 0)
    {
        _previousReceiveTime = receiveTime - utils::Time::ms * 5;
    }

    _window[_tail] = PacketAggregate(packetSize, receiveTime);
    _tail = (_tail + 1) % _window.size();
    if (_head == _tail)
    {
        _head = (_head + 1) % _window.size();
    }

    uint64_t startTime = receiveTime;
    uint64_t byteCount = 0;
    for (size_t i = 1; i <= _window.size(); ++i)
    {
        auto& p = _window[(_window.size() + _tail - i) % _window.size()];
        if (receiveTime - p.receiveTime >= utils::Time::ms * 120)
        {
            startTime = p.receiveTime;
            break;
        }
        startTime = p.receiveTime;
        byteCount += p.size;
    }
    if (receiveTime - startTime > 50 * utils::Time::ms)
    {
        const uint64_t bw = byteCount * 8 * utils::Time::ms / (receiveTime - startTime);
        auto limit = LIMIT_FIRST_BUCKET;
        for (size_t i = 0; i < _buckets.size(); ++i)
        {
            if (bw < limit)
            {
                ++_buckets[i];
                break;
            }
            else if (i == _buckets.size() - 1)
            {
                ++_buckets[_buckets.size() - 1];
            }
            limit = limit * 9 / 8;
        }
    }

    if ((++_count % 16) == 0)
    {
        double total = 0;
        for (size_t i = 0; i < _buckets.size(); ++i)
        {
            _buckets[i] = _buckets[i] * 0.9;
            total += _buckets[i];
        }

        _bucketTotal = total;
    }
}

double BwBurstTracker::getBandwidthPercentile(double percentile)
{
    double count = 0;
    auto limit = LIMIT_FIRST_BUCKET;
    if (_bucketTotal == 0)
    {
        return LIMIT_FIRST_BUCKET;
    }

    for (size_t i = 0; i < _buckets.size(); ++i)
    {
        count += _buckets[i];
        if (count >= _bucketTotal * percentile)
        {
            break;
        }
        limit = limit * 9 / 8;
    }

    _previousReport += 0.05 * (limit - _previousReport);
    return _previousReport;
}

} // namespace bwe