#include "FakeEndpointFactory.h"
#include "FakeRecordingEndpoint.h"
#include "FakeTcpServerEndpoint.h"
#include "FakeUdpEndpoint.h"
#include <memory>

namespace emulator
{

FakeEndpointFactory::FakeEndpointFactory(std::shared_ptr<fakenet::Gateway> network, EndpointCallback callback)
    : _network(network),
      _callback(callback)
{
}

transport::UdpEndpoint* FakeEndpointFactory::createUdpEndpoint(jobmanager::JobManager& jobManager,
    size_t maxSessionCount,
    memory::PacketPoolAllocator& allocator,
    const transport::SocketAddress& localPort,
    transport::RtcePoll& epoll,
    bool isShared)
{
    auto endpoint =
        new emulator::FakeUdpEndpoint(jobManager, maxSessionCount, allocator, localPort, epoll, isShared, _network);

    _callback(endpoint->getDownlink(), localPort, endpoint->getName());

    return endpoint;
}

transport::TcpEndpoint* FakeEndpointFactory::createTcpEndpoint(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    const transport::SocketAddress& nic,
    transport::RtcePoll& epoll)
{
    transport::SocketAddress local(nic.getSockAddr(), "localtcp");
    auto endpoint = new FakeTcpEndpoint(jobManager, allocator, local, _network);
    return endpoint;
}

transport::TcpEndpoint* FakeEndpointFactory::createTcpEndpoint(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    transport::RtcePoll& epoll,
    int fd,
    const transport::SocketAddress& localPort,
    const transport::SocketAddress& peerPort)
{
    assert(false);
    return nullptr;
}

transport::ServerEndpoint* FakeEndpointFactory::createTcpServerEndpoint(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    transport::RtcePoll& epoll,
    uint32_t acceptBacklog,
    transport::TcpEndpointFactory* transportFactory,
    const transport::SocketAddress& localPort,
    const config::Config& config)
{
    auto serverEndpoint = new FakeTcpServerEndpoint(jobManager,
        allocator,
        acceptBacklog,
        transportFactory,
        localPort,
        config,
        _network,
        *this,
        epoll);

    _callback(nullptr, localPort, serverEndpoint->getName());

    return serverEndpoint;
}

transport::RecordingEndpoint* FakeEndpointFactory::createRecordingEndpoint(jobmanager::JobManager& jobManager,
    size_t maxSessionCount,
    memory::PacketPoolAllocator& allocator,
    const transport::SocketAddress& localPort,
    transport::RtcePoll& epoll,
    bool isShared)
{
    return new FakeRecordingEndpoint();
}

} // namespace emulator
