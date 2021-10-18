#pragma once

#include "memory/Packet.h"
#include "utils/ByteOrder.h"

namespace recp
{
enum class AckType : uint8_t
{
    EventAck = 0x01,
    RtpNack = 0x02
};

const size_t REC_CONTROL_HEADER_SIZE = 4;

struct RecControlHeader
{
    uint8_t id;
    AckType ackType;
    nwuint16_t sequenceNumber;

    static RecControlHeader* fromPtr(void* pointer, size_t length)
    {
        assert((intptr_t)pointer % alignof(RecControlHeader) == 0);
        assert(length >= REC_CONTROL_HEADER_SIZE);
        return reinterpret_cast<RecControlHeader*>(pointer);
    }

    static RecControlHeader* fromPacket(memory::Packet& packet) { return fromPtr(packet.get(), packet.getLength()); }

    bool isRtpNack() const { return ackType == AckType::RtpNack; }

    bool isEventAck() const { return ackType == AckType::EventAck; }

    uint8_t* getPayload() { return reinterpret_cast<uint8_t*>(this) + REC_CONTROL_HEADER_SIZE; }

    uint32_t getSsrc()
    {
        assert(isRtpNack());
        auto payload = getPayload();
        NetworkOrdered<uint32_t> ssrc;
        std::memcpy(&ssrc, payload, sizeof(uint32_t));
        return ssrc;
    }
};

constexpr bool isRecControlPacket(const void* buffer, const uint32_t length)
{
    return length >= REC_CONTROL_HEADER_SIZE && reinterpret_cast<const uint8_t*>(buffer)[0] == 0x01;
}

} // namespace recp
