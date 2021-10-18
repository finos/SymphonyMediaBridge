#include "NetworkLink.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "utils/Time.h"

namespace fakenet
{
bool NetworkLink::push(memory::Packet* packet, uint64_t timestamp)
{
    if (_lossRate > 0 && rand() % 1000 < _lossRate * 1000)
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
    _queue.push(packet);
    return true;
}

memory::Packet* NetworkLink::pop()
{
    if (_queue.empty())
    {
        return nullptr;
    }
    auto* packet = _queue.front();
    _queue.pop();
    _queuedBytes -= packet->getLength();
    return packet;
}

memory::Packet* NetworkLink::pop(uint64_t timestamp)
{
    if (!_queue.empty() && static_cast<int64_t>(timestamp - _releaseTime) >= 0)
    {
        if (_bandwidthKbps == 0)
        {
            _releaseTime += utils::Time::ms * 20; // let see what it is like then
            _bitRate.update(0, timestamp);
            return nullptr;
        }

        auto* packet = _queue.front();
        _queue.pop();
        _queuedBytes -= packet->getLength();
        if (!_queue.empty())
        {
            _releaseTime =
                _releaseTime + (IPOVERHEAD + _queue.front()->getLength()) * 8 * utils::Time::ms / _bandwidthKbps;
        }
        _bitRate.update(packet->getLength() * 8, timestamp);
        return packet;
    }
    return nullptr;
}

int64_t NetworkLink::timeToRelease(uint64_t timestamp) const
{
    return empty() ? 60 * utils::Time::sec : std::max(int64_t(0), static_cast<int64_t>(_releaseTime - timestamp));
}

void NetworkLink::injectDelaySpike(uint32_t ms)
{
    _releaseTime += ms * utils::Time::ms;
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