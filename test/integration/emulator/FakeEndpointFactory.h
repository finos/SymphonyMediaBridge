#pragma once
#include "test/transport/FakeNetwork.h"
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
}

namespace emulator
{

class FakeEndpointFactory : public transport::EndpointFactory
{
public:
    FakeEndpointFactory(std::shared_ptr<fakenet::Gateway>);

    virtual transport::UdpEndpoint* createUdpEndpoint(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const transport::SocketAddress& localPort,
        transport::RtcePoll& epoll,
        bool isShared) override;

private:
    std::shared_ptr<fakenet::Gateway> _network;
};
} // namespace emulator