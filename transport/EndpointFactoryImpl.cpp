#include "transport/EndpointFactoryImpl.h"
#include "transport/RecordingEndpoint.h"
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

TcpEndpoint* EndpointFactoryImpl::createTcpEndpoint(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    RtcePoll& epoll,
    int fd,
    const SocketAddress& localPort,
    const transport::SocketAddress& peerPort)
{

    auto endpoint = new TcpEndpointImpl(jobManager, allocator, epoll, fd, localPort, peerPort);
    // if read event is fired now we may miss it and it is edge triggered.
    // everything relies on that the ice session will want to respond to the request

    epoll.add(fd, endpoint);
    return endpoint;
}

ServerEndpoint* EndpointFactoryImpl::createTcpServerEndpoint(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    RtcePoll& epoll,
    uint32_t acceptBacklog,
    TcpEndpointFactory* transportFactory,
    const SocketAddress& localPort,
    const config::Config& config)
{
    return new TcpServerEndpoint(jobManager, allocator, epoll, acceptBacklog, *transportFactory, localPort, config);
}

RecordingEndpoint* EndpointFactoryImpl::createRecordingEndpoint(jobmanager::JobManager& jobManager,
    size_t maxSessionCount,
    memory::PacketPoolAllocator& allocator,
    const SocketAddress& localPort,
    RtcePoll& epoll,
    bool isShared)
{
    return new RecordingEndpoint(jobManager, 1024, allocator, localPort, epoll, isShared);
}

} // namespace transport
