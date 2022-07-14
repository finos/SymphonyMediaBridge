#pragma once
#include "ice/IceSession.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/EndpointMetrics.h"
#include <functional>
#include <memory>

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
        STOPPING // goes to CREATED afterwards
    };
    class IEvents
    {
    public:
        virtual void onRtpReceived(Endpoint& endpoint,
            const SocketAddress& source,
            const SocketAddress& target,
            memory::UniquePacket packet) = 0;

        virtual void onDtlsReceived(Endpoint& endpoint,
            const SocketAddress& source,
            const SocketAddress& target,
            memory::UniquePacket packet) = 0;

        virtual void onRtcpReceived(Endpoint& endpoint,
            const SocketAddress& source,
            const SocketAddress& target,
            memory::UniquePacket packet) = 0;

        virtual void onIceReceived(Endpoint& endpoint,
            const SocketAddress& source,
            const SocketAddress& target,
            memory::UniquePacket packet) = 0;

        virtual void onRegistered(Endpoint& endpoint) = 0;
        virtual void onUnregistered(Endpoint& endpoint) = 0;
    };

    class IStopEvents
    {
    public:
        virtual void onEndpointStopped(Endpoint* endpoint) = 0;
    };

    virtual ~Endpoint(){};

    virtual void sendTo(const transport::SocketAddress& target, memory::UniquePacket packet) = 0;

    virtual void registerListener(const std::string& stunUserName, IEvents* listener) = 0;
    virtual void registerListener(const SocketAddress& remotePort, IEvents* listener) = 0;
    virtual void registerDefaultListener(IEvents* defaultListener) = 0;

    virtual void unregisterListener(IEvents* listener) = 0;

    virtual void start() = 0;
    virtual void stop(IStopEvents* listener) = 0;

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
        virtual void onServerPortRegistered(ServerEndpoint& endpoint) = 0;
        virtual void onServerPortUnregistered(ServerEndpoint& endpoint) = 0;

        virtual void onIceTcpConnect(std::shared_ptr<Endpoint> endpoint,
            const SocketAddress& source,
            const SocketAddress& target,
            memory::UniquePacket packet) = 0;
    };

    class IStopEvents
    {
    public:
        virtual void onEndpointStopped(ServerEndpoint* endpoint) = 0;
    };

    virtual const SocketAddress getLocalPort() const = 0;
    virtual void registerListener(const std::string& stunUserName, IEvents* listener) = 0;
    virtual void unregisterListener(const std::string& stunUserName, IEvents* listener) = 0;
    virtual void stop(IStopEvents* event) = 0;
    virtual const char* getName() const = 0;
    virtual Endpoint::State getState() const = 0;
    virtual void maintenance(uint64_t timestamp) = 0;
};

class TcpEndpointFactory
{
public:
    virtual std::shared_ptr<Endpoint> createTcpEndpoint(const transport::SocketAddress& baseAddress) = 0;
    virtual std::vector<std::shared_ptr<Endpoint>> createTcpEndpoints(int ipFamily) = 0;
    virtual std::shared_ptr<Endpoint> createTcpEndpoint(int fd,
        const transport::SocketAddress& localPort,
        const transport::SocketAddress& peerPort) = 0;
};
} // namespace transport
