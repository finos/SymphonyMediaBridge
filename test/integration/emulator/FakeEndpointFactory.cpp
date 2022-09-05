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
} // namespace emulator