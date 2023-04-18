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

    transport::UdpEndpoint* createUdpEndpoint(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const transport::SocketAddress& localPort,
        transport::RtcePoll& epoll,
        bool isShared) override;

    transport::TcpEndpoint* createTcpEndpoint(jobmanager::JobManager& jobManager,
        memory::PacketPoolAllocator& allocator,
        const transport::SocketAddress& localPort,
        transport::RtcePoll& epoll) override;

    transport::TcpEndpoint* createTcpEndpoint(jobmanager::JobManager& jobManager,
        memory::PacketPoolAllocator& allocator,
        transport::RtcePoll& epoll,
        int fd,
        const transport::SocketAddress& localPort,
        const transport::SocketAddress& peerPort) override;

    transport::ServerEndpoint* createTcpServerEndpoint(jobmanager::JobManager& jobManager,
        memory::PacketPoolAllocator& allocator,
        transport::RtcePoll& epoll,
        uint32_t acceptBacklog,
        transport::TcpEndpointFactory* transportFactory,
        const transport::SocketAddress& localPort,
        const config::Config& config) override;

    transport::RecordingEndpointImpl* createRecordingEndpoint(jobmanager::JobManager& jobManager,
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
