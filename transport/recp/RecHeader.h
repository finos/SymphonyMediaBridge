#pragma once

#include "memory/Packet.h"
#include "utils/ByteOrder.h"

namespace recp
{

enum class RecEventType : uint8_t
{
    StartStop = 0x01,
    StreamAdded = 0x02,
    StreamRemoved = 0x03,
    DominantSpeakerUpdate = 0x04,
};

const size_t REC_HEADER_SIZE = 8;

struct RecHeader
{
    // Empty first 1 byte to be easy to distinguish between RTP, RTCP, DTLS, etc..
    uint8_t empty;
    RecEventType event;
    nwuint16_t sequenceNumber;
    nwuint32_t timestamp;

    static RecHeader* fromPtr(void* pointer, size_t len)
    {
        assert((intptr_t)pointer % alignof(RecHeader) == 0);
        assert(len >= REC_HEADER_SIZE);
        static_assert(sizeof(RecHeader) == REC_HEADER_SIZE, "RecHeader size does not match REC_HEADER_SIZE");
        return reinterpret_cast<RecHeader*>(pointer);
    }

    static RecHeader* fromPacket(memory::Packet& packet) { return fromPtr(packet.get(), packet.getLength()); }

    uint8_t* getPayload() { return reinterpret_cast<uint8_t*>(&timestamp + 1); }
};

constexpr bool isRecPacket(const void* buffer, const uint32_t length)
{
    return length >= REC_HEADER_SIZE && reinterpret_cast<const uint8_t*>(buffer)[0] == 0x00;
}

inline bool isRecPacket(const memory::Packet& packet)
{
    return isRecPacket(packet.get(), packet.getLength());
}

} // namespace recp
