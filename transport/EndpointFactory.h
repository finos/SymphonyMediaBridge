#pragma once
#include <memory>

#include "transport/UdpEndpointImpl.h"

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

class EndpointFactory
{
public:
    virtual UdpEndpoint* createUdpEndpoint(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const SocketAddress& localPort,
        RtcePoll& epoll,
        bool isShared) = 0;
    virtual ~EndpointFactory(){};
};
} // namespace transport