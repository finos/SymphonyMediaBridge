#pragma once
#include "memory/PacketPoolAllocator.h"

namespace rtp
{
struct RtpHeader;
/**
 * Effective jitter buffer. Packets are stored in linked list. Ordered packets are quickly added to the end of the list.
 * Rarely occurring out of order packets have to be inserted in the list after a quick scan.
 */

class JitterBufferList
{
public:
    enum
    {
        SIZE = 300
    };

    JitterBufferList();

    bool add(memory::UniquePacket packet);
    memory::UniquePacket pop();
    uint32_t getRtpDelay() const;
    int32_t getRtpDelay(uint32_t rtpTimestamp) const;
    const rtp::RtpHeader* getFrontRtp() const;
    const rtp::RtpHeader* getTailRtp() const;

    bool empty() const { return !_head; }
    uint32_t count() const { return _count; }

private:
    struct ListItem
    {
        memory::UniquePacket packet;
        ListItem* next = nullptr;
    };

    ListItem* allocItem();

    ListItem* _freeItems;
    ListItem _itemStore[SIZE];
    ListItem* _head;
    ListItem* _tail;
    uint32_t _count;
};
} // namespace rtp
