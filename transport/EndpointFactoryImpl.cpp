#include "transport/EndpointFactoryImpl.h"
#include "transport/TcpEndpointImpl.h"
#include "transport/UdpEndpointImpl.h"
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

TcpEndpoint* EndpointFactoryImpl::createTcpEndpoint(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    const SocketAddress& nic,
    RtcePoll& epoll)
{
    return new TcpEndpointImpl(jobManager, allocator, nic, epoll);
}

TcpServerEndpoint* EndpointFactoryImpl::createTcpServerEndpoint(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    RtcePoll& epoll,
    uint32_t acceptBacklog,
    TcpEndpointFactory* transportFactory,
    const SocketAddress& localPort,
    const config::Config& config)
{
    return new TcpServerEndpoint(jobManager, allocator, epoll, acceptBacklog, *transportFactory, localPort, config);
}

} // namespace transport
