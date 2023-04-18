#include "FakeEndpointImpl.h"
#include "utils/SocketAddress.h"

namespace emulator
{
memory::UniquePacket serializeInbound(memory::PacketPoolAllocator& allocator,
    const transport::SocketAddress& source,
    const void* data,
    size_t length)
{
    memory::FixedPacket<2000> packet;
    auto protocol = ProtocolIndicator::UDP;
    packet.append(&protocol, 1);
    packet.append(source.getSockAddr(), source.getSockAddrSize());
    packet.append(data, length);

    return memory::makeUniquePacket(allocator, packet.get(), packet.getLength());
}

InboundPacket deserializeInbound(memory::PacketPoolAllocator& allocator, memory::UniquePacket packet)
{
    const auto* packetBuf = reinterpret_cast<const uint8_t*>(packet->get());
    const ProtocolIndicator protocol = static_cast<ProtocolIndicator>(*packetBuf);
    const transport::SocketAddress source(reinterpret_cast<const sockaddr*>(&packetBuf[1]));
    const size_t prefixLength = 1 + source.getSockAddrSize();

    if (protocol == ProtocolIndicator::UDP || protocol == ProtocolIndicator::TCPDATA)
    {
        return {protocol,
            source,
            memory::makeUniquePacket(allocator, packetBuf + prefixLength, packet->getLength() - prefixLength)};
    }
    else
    {
        return {protocol, source, memory::makeUniquePacket(allocator, packetBuf + prefixLength, 0)};
    }
}

} // namespace emulator
