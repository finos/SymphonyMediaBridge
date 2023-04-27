#include "FakeEndpointImpl.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "transport/dtls/SslDtls.h"
#include "transport/ice/Stun.h"
#include "utils/SocketAddress.h"

namespace emulator
{
memory::UniquePacket serializeInbound(memory::PacketPoolAllocator& allocator,
    fakenet::Protocol protocol,
    const transport::SocketAddress& source,
    const void* data,
    size_t length)
{
    memory::FixedPacket<2000> packet;
    packet.append(&protocol, 1);
    packet.append(source.getSockAddr(), source.getSockAddrSize());
    packet.append(data, length);

    return memory::makeUniquePacket(allocator, packet.get(), packet.getLength());
}

InboundPacket deserializeInbound(memory::PacketPoolAllocator& allocator, memory::UniquePacket packet)
{
    const uint8_t* packetBuf = reinterpret_cast<const uint8_t*>(packet->get());
    auto protocol = static_cast<fakenet::Protocol>(packetBuf[0]);
    const transport::SocketAddress source(reinterpret_cast<const sockaddr*>(&packetBuf[1]));
    const size_t prefixLength = 1 + source.getSockAddrSize();

    return {protocol,
        source,
        memory::makeUniquePacket(allocator, packetBuf + prefixLength, packet->getLength() - prefixLength)};
}

bool isWeirdPacket(memory::Packet& packet)
{
    if (!ice::isStunMessage(packet.get(), packet.getLength()) && !rtp::isRtpPacket(packet) &&
        !rtp::isRtcpPacket(packet) && !transport::isDtlsPacket(packet.get(), packet.getLength()) &&
        packet.getLength() > 0)
    {
        return true;
    }
    return false;
}

} // namespace emulator
