#include "NetworkLink.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "utils/Time.h"

#define TRACE_FAKENETWORK 1

#if TRACE_FAKENETWORK
#define NETWORK_LOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
#define NETWORK_LOG(fmt, ...)
#endif

namespace fakenet
{

bool NetworkLink::push(memory::UniquePacket packet, uint64_t timestamp, bool tcpData)
{
    const std::lock_guard<std::mutex> lock(_pushMutex);

    if (!tcpData && _lossRate > 0 && rand() % 1000 < _lossRate * 1000)
    {
        logger::debug("dropping packet", "");
        return false;
    }

    if (_queue.empty())
    {
        if (_bandwidthKbps == 0)
        {
            _releaseTime = timestamp + utils::Time::ms * 20;
        }
        else if (_burstIntervalUs > 0)
        {
            _releaseTime = timestamp + ((IPOVERHEAD + packet->getLength()) * 8 * utils::Time::ms / _bandwidthKbps);
            addBurstDelay();
        }
        else
        {
            _releaseTime = timestamp + ((IPOVERHEAD + packet->getLength()) * 8 * utils::Time::ms / _bandwidthKbps);
        }
    }

    if (packet->getLength() > _mtu || packet->getLength() + _queuedBytes > _bufferCapacity)
    {
        return false;
    }
    _queuedBytes += packet->getLength();
    _queue.push(std::move(packet));
    return true;
}

memory::UniquePacket NetworkLink::pop()
{
    const std::lock_guard<std::mutex> lock(_pushMutex);
    if (!_delayQueue.empty())
    {
        memory::UniquePacket packet(std::move(_delayQueue.front().packet));
        _delayQueue.pop();
        return packet;
    }

    if (!_queue.empty())
    {
        memory::UniquePacket packet(std::move(_queue.front()));
        _queue.pop();
        _queuedBytes -= packet->getLength();
        return packet;
    }
    return memory::UniquePacket();
}

memory::UniquePacket NetworkLink::pop(uint64_t timestamp)
{
    const std::lock_guard<std::mutex> lock(_pushMutex);
    if (!_queue.empty() && static_cast<int64_t>(timestamp - _releaseTime) >= 0)
    {
        if (_bandwidthKbps == 0)
        {
            _releaseTime += utils::Time::ms * 20; // let see what it is like then
            _bitRate.update(0, timestamp);
            return popDelayQueue(timestamp);
        }

        memory::UniquePacket packet(std::move(_queue.front()));
        _queue.pop();
        _queuedBytes -= packet->getLength();
        if (!_queue.empty())
        {
            _releaseTime =
                _releaseTime + (IPOVERHEAD + _queue.front()->getLength()) * 8 * utils::Time::ms / _bandwidthKbps;
        }
        _bitRate.update(packet->getLength() * 8, timestamp);
        _delayQueue.push({std::move(packet), timestamp + _staticDelay});
    }
    return popDelayQueue(timestamp);
}

memory::UniquePacket NetworkLink::popDelayQueue(uint64_t timestamp)
{
    if (!_delayQueue.empty() && utils::Time::diffLE(timestamp, _delayQueue.front().releaseTime, 0))
    {
        memory::UniquePacket packet(std::move(_delayQueue.front().packet));
        _delayQueue.pop();
        return packet;
    }
    return nullptr;
}

int64_t NetworkLink::timeToRelease(uint64_t timestamp) const
{
    int64_t period = 60 * utils::Time::sec;
    if (!_delayQueue.empty())
    {
        period = std::max(int64_t(0), static_cast<int64_t>(_delayQueue.front().releaseTime - timestamp));
    }
    if (!_queue.empty())
    {
        const auto period2 = std::max(int64_t(0), static_cast<int64_t>(_releaseTime - timestamp));
        period = std::min(period, period2);
    }
    return period;
}

void NetworkLink::injectDelaySpike(uint32_t ms)
{
    _releaseTime += ms * utils::Time::ms;
}

void NetworkLink::setStaticDelay(uint32_t ms)
{
    NETWORK_LOG("setting static delay to %ums", _name.c_str(), ms);
    _staticDelay = ms * utils::Time::ms;
}

void NetworkLink::setBurstDeliveryInterval(uint32_t ms)
{
    _burstIntervalUs = ms * 1000;
}

void NetworkLink::addBurstDelay()
{
    if (_burstIntervalUs > 0)
    {
        uint64_t burstDelay = 1000 * (_burstIntervalUs * 9 / 10 + rand() % (_burstIntervalUs / 5));
        _releaseTime += burstDelay;
    }
}

} // namespace fakenet
