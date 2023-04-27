#pragma once
#include <memory>

#include "transport/TcpServerEndpoint.h"
#include "transport/UdpEndpoint.h"

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
class TcpEndpoint;
class UdpEndpoint;
class ServerEndpoint;
class RecordingEndpoint;

class EndpointFactory
{
public:
    virtual ~EndpointFactory(){};

    virtual UdpEndpoint* createUdpEndpoint(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const SocketAddress& localPort,
        RtcePoll& epoll,
        bool isShared) = 0;

    virtual TcpEndpoint* createTcpEndpoint(jobmanager::JobManager& jobManager,
        memory::PacketPoolAllocator& allocator,
        const SocketAddress& localPort,
        RtcePoll& epoll) = 0;

    virtual TcpEndpoint* createTcpEndpoint(jobmanager::JobManager& jobManager,
        memory::PacketPoolAllocator& allocator,
        RtcePoll& epoll,
        int fd,
        const SocketAddress& localPort,
        const transport::SocketAddress& peerPort) = 0;

    virtual ServerEndpoint* createTcpServerEndpoint(jobmanager::JobManager& jobManager,
        memory::PacketPoolAllocator& allocator,
        RtcePoll& epoll,
        uint32_t acceptBacklog,
        TcpEndpointFactory* transportFactory,
        const SocketAddress& localPort,
        const config::Config& config) = 0;

    virtual RecordingEndpoint* createRecordingEndpoint(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const SocketAddress& localPort,
        RtcePoll& epoll,
        bool isShared) = 0;
};
} // namespace transport
