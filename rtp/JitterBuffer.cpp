#include "rtp/JitterBuffer.h"
#include "rtp/RtpHeader.h"

namespace rtp
{

JitterBuffer::JitterBuffer() : _head(0), _tail(0), _count(0) {}

JitterBuffer::~JitterBuffer() {}

bool JitterBuffer::add(memory::UniquePacket packet)
{
    const auto newHeader = RtpHeader::fromPacket(*packet);
    if (!newHeader)
    {
        return false; // corrupt
    }

    if (empty())
    {
        const auto pos = newHeader->sequenceNumber.get() % SIZE;
        _items[pos] = std::move(packet);
        _head = pos;
        _tail = (pos + 1) % SIZE;
        ++_count;
        return true;
    }

    const auto frontHeader = getFrontRtp();
    const auto pos = newHeader->sequenceNumber.get() % SIZE;
    const auto relativeHead = static_cast<int16_t>(newHeader->sequenceNumber.get() - frontHeader->sequenceNumber.get());
    if (relativeHead < 0 && -relativeHead >= static_cast<int>(SIZE - 1 - sequenceSpan()))
    {
        return false; // cannot insert because it would be outside range
    }
    else if (relativeHead > SIZE - 2)
    {
        return false; // cannot extend range
    }

    _items[pos] = std::move(packet);
    ++_count;
    if (relativeHead < 0)
    {
        _head = pos;
    }
    else if (static_cast<uint32_t>(relativeHead) >= sequenceSpan())
    {
        _tail = (pos + 1) % SIZE;
    }

    return true;
}

memory::UniquePacket JitterBuffer::pop()
{
    if (empty())
    {
        return nullptr;
    }

    for (uint32_t i = _head; i != _tail; i = (i + 1) % SIZE)
    {
        if (_items[i])
        {
            for (_head = (_head + 1) % SIZE; !_items[_head] && _head != _tail; _head = (_head + 1) % SIZE) {}
            --_count;
            return std::move(_items[i]);
        }
    }

    return nullptr;
}

uint32_t JitterBuffer::getRtpDelay() const
{
    if (empty())
    {
        return 0;
    }

    const auto headHeader = getFrontRtp();
    const auto tailHeader = getTailRtp();
    return tailHeader->timestamp.get() - headHeader->timestamp.get();
}

int32_t JitterBuffer::getRtpDelay(uint32_t rtpTimestamp) const
{
    if (empty())
    {
        return 0;
    }

    return static_cast<int32_t>(getTailRtp()->timestamp.get() - rtpTimestamp);
}

const rtp::RtpHeader* JitterBuffer::getFrontRtp() const
{
    if (empty())
    {
        return nullptr;
    }

    return rtp::RtpHeader::fromPacket(*_items[_head]);
}

const rtp::RtpHeader* JitterBuffer::getTailRtp() const
{
    if (empty())
    {
        return nullptr;
    }

    return rtp::RtpHeader::fromPacket(*_items[(_tail + SIZE - 1) % SIZE]);
}

} // namespace rtp
