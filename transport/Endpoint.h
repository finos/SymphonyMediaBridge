#pragma once
#include "ice/IceSession.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/EndpointMetrics.h"
#include <functional>

namespace transport
{
class SocketAddress;
class RtcePoll;

// end point that can be shared by multiple transports and can route incoming traffic
class Endpoint : public ice::IceEndpoint
{
public:
    enum State
    {
        CLOSED = 0,
        CREATED,
        CONNECTING,
        CONNECTED,
        CLOSING
    };
    class IEvents
    {
    public:
        virtual void onRtpReceived(Endpoint& endpoint,
            const SocketAddress& source,
            const SocketAddress& target,
            memory::Packet* packet,
            memory::PacketPoolAllocator& allocator) = 0;

        virtual void onDtlsReceived(Endpoint& endpoint,
            const SocketAddress& source,
            const SocketAddress& target,
            memory::Packet* packet,
            memory::PacketPoolAllocator& allocator) = 0;

        virtual void onRtcpReceived(Endpoint& endpoint,
            const SocketAddress& source,
            const SocketAddress& target,
            memory::Packet* packet,
            memory::PacketPoolAllocator& allocator) = 0;

        virtual void onIceReceived(Endpoint& endpoint,
            const SocketAddress& source,
            const SocketAddress& target,
            memory::Packet* packet,
            memory::PacketPoolAllocator& allocator) = 0;

        virtual void onPortClosed(Endpoint& endpoint) = 0;
        virtual void onUnregistered(Endpoint& endpoint) = 0;
    };

    virtual ~Endpoint(){};

    virtual void sendTo(const transport::SocketAddress& target,
        memory::Packet* packet,
        memory::PacketPoolAllocator& allocator) = 0;

    virtual void registerListener(const std::string& stunUserName, IEvents* listener) = 0;
    virtual void registerListener(const SocketAddress& remotePort, IEvents* listener) = 0;
    virtual void registerDefaultListener(IEvents* defaultListener) = 0;

    virtual void unregisterListener(IEvents* listener) = 0;

    virtual void start() = 0;
    virtual void closePort() = 0;

    virtual bool configureBufferSizes(size_t sendBufferSize, size_t receiveBufferSize) = 0;

    virtual bool isShared() const = 0;

    virtual const char* getName() const = 0;
    virtual State getState() const = 0;

    virtual EndpointMetrics getMetrics(uint64_t timestamp) const = 0;
};

class ServerEndpoint
{
public:
    virtual ~ServerEndpoint() {}
    class IEvents
    {
    public:
        virtual void onServerPortClosed(ServerEndpoint& endpoint) = 0;
        virtual void onServerPortUnregistered(ServerEndpoint& endpoint) = 0;
    };

    virtual const SocketAddress getLocalPort() const = 0;
    virtual void registerListener(const std::string& stunUserName, Endpoint::IEvents* listener) = 0;
    virtual void unregisterListener(const std::string& stunUserName, IEvents* listener) = 0;
    virtual void close() = 0;
    virtual const char* getName() const = 0;
    virtual Endpoint::State getState() const = 0;
};

class TcpEndpointFactory
{
public:
    virtual Endpoint* createTcpEndpoint(const transport::SocketAddress& baseAddress) = 0;
    virtual std::vector<Endpoint*> createTcpEndpoints(int ipFamily) = 0;
};
} // namespace transport
