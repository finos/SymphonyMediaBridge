#include "FakeEndpointFactory.h"
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
    _network->addLocal(static_cast<fakenet::NetworkNode*>(endpoint));

    _callback(endpoint->getDownlink(), localPort, endpoint->getName());

    return static_cast<transport::UdpEndpoint*>(endpoint);
}

transport::TcpEndpoint* FakeEndpointFactory::createTcpEndpoint(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    const transport::SocketAddress& localPort,
    transport::RtcePoll& epoll)
{
    return nullptr;
}

transport::TcpServerEndpoint* FakeEndpointFactory::createTcpServerEndpoint(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    transport::RtcePoll& epoll,
    uint32_t acceptBacklog,
    transport::TcpEndpointFactory* transportFactory,
    const transport::SocketAddress& localPort,
    const config::Config& config)
{
    return nullptr;
}

} // namespace emulator
