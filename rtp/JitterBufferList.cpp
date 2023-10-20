#include "rtp/JitterBufferList.h"
#include "rtp/RtpHeader.h"
#include <queue>

namespace rtp
{

JitterBufferList::JitterBufferList(size_t maxLength)
    : _freeItems(nullptr),
      _itemStore(new ListItem[maxLength]),
      _head(nullptr),
      _tail(nullptr),
      _count(0)
{
    for (size_t i = 0; i < maxLength; ++i)
    {
        _itemStore[i].next = _freeItems;
        _freeItems = &_itemStore[i];
    }
}

JitterBufferList::~JitterBufferList()
{
    delete[] _itemStore;
}

JitterBufferList::ListItem* JitterBufferList::allocItem()
{
    if (!_freeItems)
    {
        return nullptr;
    }

    auto item = _freeItems;
    _freeItems = item->next;
    item->next = nullptr;
    return item;
}

bool JitterBufferList::add(memory::UniquePacket packet)
{
    const auto newHeader = RtpHeader::fromPacket(*packet);
    if (!newHeader)
    {
        return false; // corrupt
    }

    auto newItem = allocItem();
    if (!newItem)
    {
        return false; // full
    }

    newItem->packet = std::move(packet);
    ++_count;

    if (_tail)
    {
        const auto tailHeader = RtpHeader::fromPacket(*_tail->packet);
        if (static_cast<int16_t>(newHeader->sequenceNumber.get() - tailHeader->sequenceNumber.get()) >= 0)
        {
            _tail->next = newItem;
            _tail = newItem;
            return true;
        }
    }
    else
    {
        _head = newItem;
        _tail = newItem;
        return true;
    }

    for (ListItem* item = _head; item; item = item->next)
    {
        const auto itemHeader = RtpHeader::fromPacket(*item->next->packet);
        if (static_cast<int16_t>(itemHeader->sequenceNumber.get() - newHeader->sequenceNumber.get()) > 0)
        {
            newItem->next = item->next;
            item->next = newItem;
            if (item == _head)
            {
                _head = newItem;
            }
            return true;
        }
    }

    return false;
}

memory::UniquePacket JitterBufferList::pop()
{
    if (!_head)
    {
        return nullptr;
    }

    auto item = _head;
    _head = item->next;
    if (_tail == item)
    {
        _tail = nullptr;
    }

    item->next = _freeItems;
    _freeItems = item;
    --_count;
    return std::move(item->packet);
}

uint32_t JitterBufferList::getRtpDelay() const
{
    if (!_tail)
    {
        return 0;
    }

    const auto headHeader = rtp::RtpHeader::fromPacket(*_head->packet);
    const auto tailHeader = rtp::RtpHeader::fromPacket(*_tail->packet);
    return tailHeader->timestamp.get() - headHeader->timestamp.get();
}

int32_t JitterBufferList::getRtpDelay(uint32_t rtpTimestamp) const
{
    if (!_tail)
    {
        return 0;
    }

    const auto tailHeader = rtp::RtpHeader::fromPacket(*_tail->packet);
    return static_cast<int32_t>(tailHeader->timestamp.get() - rtpTimestamp);
}

const rtp::RtpHeader* JitterBufferList::getFrontRtp() const
{
    if (!_head)
    {
        return nullptr;
    }

    return rtp::RtpHeader::fromPacket(*_head->packet);
}

const rtp::RtpHeader* JitterBufferList::getTailRtp() const
{
    if (!_tail)
    {
        return nullptr;
    }

    return rtp::RtpHeader::fromPacket(*_tail->packet);
}

} // namespace rtp
