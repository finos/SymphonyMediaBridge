#pragma once
#include "memory/PacketPoolAllocator.h"
#include "test/transport/FakeNetwork.h"
#include "transport/EndpointMetrics.h"
#include "utils/SocketAddress.h"

namespace emulator
{

enum ProtocolIndicator : uint8_t
{
    UDP = 0,
    SYN,
    FIN,
    ACK,
    TCPDATA
};

struct InboundPacket
{
    ProtocolIndicator protocol;
    transport::SocketAddress address;
    memory::UniquePacket packet;
};

struct OutboundPacket
{
    transport::SocketAddress sourceAddress;
    transport::SocketAddress targetAddress;
    memory::UniquePacket packet;
};

memory::UniquePacket serializeInbound(memory::PacketPoolAllocator& allocator,
    const transport::SocketAddress& source,
    const void* data,
    size_t length);

InboundPacket deserializeInbound(memory::PacketPoolAllocator& allocator, memory::UniquePacket packet);

class FakeEndpointImpl : public fakenet::NetworkNode
{
public:
protected:
    struct RateMetrics
    {
        utils::TrackerWithSnapshot<10, utils::Time::ms * 100, utils::Time::sec> receiveTracker;
        utils::TrackerWithSnapshot<10, utils::Time::ms * 100, utils::Time::sec> sendTracker;
        EndpointMetrics toEndpointMetrics(size_t queueSize) const
        {
            return EndpointMetrics(queueSize,
                receiveTracker.snapshot.load() * 8 * utils::Time::ms,
                sendTracker.snapshot.load() * 8 * utils::Time::ms,
                0);
        }
    } _rateMetrics;
};
} // namespace emulator
