#include "transport/EndpointFactoryImpl.h"
#include <memory>

namespace transport
{
UdpEndpoint* EndpointFactoryImpl::createUdpEndpointStatic(jobmanager::JobManager& jobManager,
    size_t maxSessionCount,
    memory::PacketPoolAllocator& allocator,
    const SocketAddress& localPort,
    RtcePoll& epoll,
    bool isShared)
{
    return static_cast<UdpEndpoint*>(
        new UdpEndpointImpl(jobManager, maxSessionCount, allocator, localPort, epoll, isShared));
}

UdpEndpoint* EndpointFactoryImpl::createUdpEndpoint(jobmanager::JobManager& jobManager,
    size_t maxSessionCount,
    memory::PacketPoolAllocator& allocator,
    const SocketAddress& localPort,
    RtcePoll& epoll,
    bool isShared)
{
    return createUdpEndpointStatic(jobManager, maxSessionCount, allocator, localPort, epoll, isShared);
}
} // namespace transport