#pragma once
#include "memory/PacketPoolAllocator.h"
#include "test/transport/FakeNetwork.h"
#include "transport/EndpointMetrics.h"
#include "utils/SocketAddress.h"

namespace emulator
{

struct InboundPacket
{
    fakenet::Protocol protocol;
    transport::SocketAddress source;
    memory::UniquePacket packet;
};

struct OutboundPacket
{
    fakenet::Protocol protocol;
    transport::SocketAddress sourceAddress;
    transport::SocketAddress targetAddress;
    memory::UniquePacket packet;
};

memory::UniquePacket serializeInbound(memory::PacketPoolAllocator& allocator,
    fakenet::Protocol protocol,
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

bool isWeirdPacket(memory::Packet& packet);
} // namespace emulator
