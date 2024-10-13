#pragma once
#include "concurrency/MpmcHashmap.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/BaseUdpEndpoint.h"

namespace transport
{
class EndpointFactoryImpl;

// end point that can be shared by multiple transports and can route incoming traffic
class UdpEndpointImpl : public UdpEndpoint
{
    friend class EndpointFactoryImpl;
    UdpEndpointImpl(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const SocketAddress& localPort,
        RtcePoll& epoll,
        bool isShared);

public:
    ~UdpEndpointImpl();

    void sendStunTo(const transport::SocketAddress& target,
        ice::Int96 transactionId,
        const void* data,
        size_t len,
        uint64_t timestamp) override;

    void cancelStunTransaction(ice::Int96 transactionId) override;

    void registerListener(const std::string& stunUserName, IEvents* listener) override;
    void registerListener(const SocketAddress& remotePort, IEvents* listener) override;

    void unregisterListener(IEvents* listener) override;
    void unregisterListener(const SocketAddress& remotePort, IEvents* listener) override;

    bool openPort(uint16_t port) override { return _baseUdpEndpoint.openPort(port); }
    bool isGood() const override { return _baseUdpEndpoint.isGood(); }
    ice::TransportType getTransportType() const override { return ice::TransportType::UDP; }

    virtual void sendTo(const transport::SocketAddress& target, memory::UniquePacket packet) override
    {
        _baseUdpEndpoint.sendTo(target, std::move(packet));
    }

    virtual void registerDefaultListener(IEvents* defaultListener) override { _defaultListener = defaultListener; }

    virtual void start() override { _baseUdpEndpoint.start(); }
    virtual void stop(IStopEvents* listener) override { _baseUdpEndpoint.stop(listener); }

    virtual bool configureBufferSizes(size_t sendBufferSize, size_t receiveBufferSize) override
    {
        return _baseUdpEndpoint.configureBufferSizes(sendBufferSize, receiveBufferSize);
    }

    virtual const char* getName() const override { return _name.c_str(); }
    SocketAddress getLocalPort() const override { return _baseUdpEndpoint._socket.getBoundPort(); }
    virtual State getState() const override { return _baseUdpEndpoint._state; }

    virtual EndpointMetrics getMetrics(uint64_t timestamp) const override
    {
        return _baseUdpEndpoint.getMetrics(timestamp);
    }

private:
    void dispatchReceivedPacket(const SocketAddress& srcAddress, memory::UniquePacket packet, const uint64_t timestamp);

    void internalUnregisterListener(IEvents* listener);
    void internalUnregisterStunListener(ice::Int96 transactionId);
    void swapListener(const SocketAddress& srcAddress, IEvents* newListener);

    logger::LoggableId _name;
    BaseUdpEndpoint _baseUdpEndpoint;

    concurrency::MpmcHashmap32<std::string, IEvents*> _iceListeners;
    concurrency::MpmcHashmap32<SocketAddress, IEvents*> _dtlsListeners;

    // mainly used for client requests. SMB mainly uses dtlsListener for IP:port
    concurrency::MpmcHashmap32<ice::Int96, IEvents*> _iceResponseListeners;
    std::atomic<Endpoint::IEvents*> _defaultListener;
};
} // namespace transport
