#pragma once
#include "transport/EndpointFactory.h"
#include <memory>

namespace jobmanager
{
class JobManager;
}

namespace memory
{
class PacketPoolAllocator;
}

namespace transport
{
class RtcePoll;

class EndpointFactoryImpl : public EndpointFactory
{
public:
    static UdpEndpoint* createUdpEndpointStatic(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const SocketAddress& localPort,
        RtcePoll& epoll,
        bool isShared);

    virtual UdpEndpoint* createUdpEndpoint(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const SocketAddress& localPort,
        RtcePoll& epoll,
        bool isShared) override;
};
} // namespace transport