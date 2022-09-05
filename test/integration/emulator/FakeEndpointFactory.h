#pragma once
#include "test/transport/FakeNetwork.h"
#include "test/transport/NetworkLink.h"
#include "transport/EndpointFactory.h"
#include <functional>
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
    using EndpointCallback =
        std::function<void(std::shared_ptr<fakenet::NetworkLink>, const transport::SocketAddress&, const std::string&)>;

public:
    FakeEndpointFactory(std::shared_ptr<fakenet::Gateway>, EndpointCallback);

    virtual transport::UdpEndpoint* createUdpEndpoint(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const transport::SocketAddress& localPort,
        transport::RtcePoll& epoll,
        bool isShared) override;

private:
    std::shared_ptr<fakenet::Gateway> _network;
    EndpointCallback _callback;
};
} // namespace emulator