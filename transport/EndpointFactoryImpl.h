#pragma once
#include "transport/EndpointFactory.h"
#include <memory>

namespace config
{
class Config;
}

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

class EndpointFactoryImpl : public EndpointFactory
{
public:
    static UdpEndpoint* createUdpEndpointStatic(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const SocketAddress& localPort,
        RtcePoll& epoll,
        bool isShared);

    virtual UdpEndpoint* createUdpEndpoint(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const SocketAddress& localPort,
        RtcePoll& epoll,
        bool isShared) override;

    virtual TcpEndpoint* createTcpEndpoint(jobmanager::JobManager& jobManager,
        memory::PacketPoolAllocator& allocator,
        const SocketAddress& localPort,
        RtcePoll& epoll) override;

    virtual TcpEndpoint* createTcpEndpoint(jobmanager::JobManager& jobManager,
        memory::PacketPoolAllocator& allocator,
        RtcePoll& epoll,
        int fd,
        const SocketAddress& localPort,
        const transport::SocketAddress& peerPort) override;

    virtual ServerEndpoint* createTcpServerEndpoint(jobmanager::JobManager& jobManager,
        memory::PacketPoolAllocator& allocator,
        RtcePoll& epoll,
        uint32_t acceptBacklog,
        TcpEndpointFactory* transportFactory,
        const SocketAddress& localPort,
        const config::Config& config) override;
};
} // namespace transport
