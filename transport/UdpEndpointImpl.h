#pragma once
#include "concurrency/MpmcHashmap.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/BaseUdpEndpoint.h"

namespace transport
{
class EndpointFactoryImpl;

// end point that can be shared by multiple transports and can route incoming traffic
class UdpEndpointImpl : public BaseUdpEndpoint
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
        __uint128_t transactionId,
        const void* data,
        size_t len,
        uint64_t timestamp) override;

    void cancelStunTransaction(__uint128_t transactionId) override;

    void registerListener(const std::string& stunUserName, IEvents* listener) override;
    void registerListener(const SocketAddress& remotePort, IEvents* listener) override;

    void unregisterListener(IEvents* listener) override;
    void unregisterListener(const SocketAddress& remotePort, IEvents* listener) override;

private:
    void dispatchReceivedPacket(const SocketAddress& srcAddress,
        memory::UniquePacket packet,
        uint64_t timestamp) override;

    void internalUnregisterListener(IEvents* listener);
    void internalUnregisterStunListener(__uint128_t transactionId);
    void swapListener(const SocketAddress& srcAddress, IEvents* newListener);

    concurrency::MpmcHashmap32<std::string, IEvents*> _iceListeners;
    concurrency::MpmcHashmap32<SocketAddress, IEvents*> _dtlsListeners;

    // mainly used for client requests. SMB mainly uses dtlsListener for IP:port
    concurrency::MpmcHashmap32<__uint128_t, IEvents*> _iceResponseListeners;
};
} // namespace transport
