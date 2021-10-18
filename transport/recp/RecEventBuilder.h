
#pragma once

#include "transport/recp/RecHeader.h"
#include "memory/PacketPoolAllocator.h"
#include <type_traits>

namespace recp
{

template< class TDerived, RecEventType TEvent, uint16_t PacketMinSize >
class RecEventBuilder
{
    using TThis = RecEventBuilder<TDerived, TEvent, PacketMinSize>;

protected:
    using TBaseBuilder = TThis;
    static constexpr uint16_t MinSize = PacketMinSize;

public:
    explicit RecEventBuilder(memory::PacketPoolAllocator& allocator);
    ~RecEventBuilder();

    RecEventBuilder(const TThis& rhs) = delete;
    RecEventBuilder();
    RecEventBuilder& operator=(const TThis& rhs) = delete;

    TDerived& setTimestamp(uint32_t utc);
    TDerived& setSequenceNumber(uint16_t sequenceNumber);

    memory::Packet* build();

protected:
    RecHeader* getHeader();
    memory::Packet* getPacket();

private:
    void allocateIfNeed();
    RecHeader* header() { return reinterpret_cast<RecHeader*>(_packet); }

protected:
    memory::PacketPoolAllocator& _allocator;
    memory::Packet* _packet;
};


template< class TDerived, RecEventType TEvent, ushort PacketMinSize >
RecEventBuilder< TDerived, TEvent, PacketMinSize >::RecEventBuilder(memory::PacketPoolAllocator& allocator)
    : _allocator(allocator)
    , _packet(nullptr)
{
    static_assert(std::is_base_of<TThis, TDerived>::value, "TDerived must derived from RecEventBuilder<TDerived, RecEventType, PacketMinSize>");
    static_assert(PacketMinSize >= REC_HEADER_SIZE, "Packet size must to be equal or grater than the RecHeader::MinSize");
}

template< class TDerived, RecEventType TEvent, ushort PacketMinSize >
RecEventBuilder< TDerived, TEvent, PacketMinSize >::~RecEventBuilder()
{
    if (_packet)
    {
        _allocator.free(_packet);
        _packet = nullptr;
    }
}


template< class TDerived, RecEventType TEvent, ushort PacketMinSize >
void
RecEventBuilder< TDerived, TEvent, PacketMinSize >::allocateIfNeed()
{
    if (!_packet)
    {
        _packet = memory::makePacket(_allocator);
        assert(_packet);

        _packet->setLength(PacketMinSize);
        header()->empty = 0;
        header()->event = TEvent;
        //Clean all bytes after the header
        std::memset(_packet->get() + REC_HEADER_SIZE, 0, PacketMinSize - REC_HEADER_SIZE);
    }
}

template< class TDerived, RecEventType TEvent, ushort PacketMinSize >
RecHeader*
RecEventBuilder< TDerived, TEvent, PacketMinSize >::getHeader()
{
    allocateIfNeed();
    return header();
}

template< class TDerived, RecEventType TEvent, ushort PacketMinSize >
memory::Packet*
RecEventBuilder< TDerived, TEvent, PacketMinSize >::getPacket()
{
    allocateIfNeed();
    return _packet;
}


template< class TDerived, RecEventType TEvent, ushort PacketMinSize >
TDerived&
RecEventBuilder< TDerived, TEvent, PacketMinSize >::setTimestamp(uint32_t utc)
{
    getHeader()->timestamp = utc;
    return static_cast<TDerived&>(*this);
}

template< class TDerived, RecEventType TEvent, ushort PacketMinSize >
TDerived&
RecEventBuilder< TDerived, TEvent, PacketMinSize >::setSequenceNumber(uint16_t sequenceNumber)
{
    getHeader()->sequenceNumber = sequenceNumber;
    return static_cast<TDerived&>(*this);
}

template< class TDerived, RecEventType TEvent, ushort PacketMinSize >
memory::Packet*
RecEventBuilder< TDerived, TEvent, PacketMinSize >::build()
{
    allocateIfNeed();
    auto* packet = _packet;
    _packet = nullptr;
    return packet;
}

} //namespacace recp
