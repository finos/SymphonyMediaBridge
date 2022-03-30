#pragma once
#include "concurrency/MpmcHashmap.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/BaseUdpEndpoint.h"

namespace transport
{
// end point that can be shared by multiple transports and can route incoming traffic
class UdpEndpoint : public BaseUdpEndpoint
{
public:
    UdpEndpoint(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const SocketAddress& localPort,
        RtcePoll& epoll,
        bool isShared);

    void sendStunTo(const transport::SocketAddress& target,
        __uint128_t transactionId,
        const void* data,
        size_t len,
        uint64_t timestamp) override;

    void registerListener(const std::string& stunUserName, IEvents* listener) override;
    void registerListener(const SocketAddress& remotePort, IEvents* listener) override;

    void unregisterListener(IEvents* listener) override;

public: // internal job interface
    void dispatchReceivedPacket(const SocketAddress& srcAddress, memory::Packet* packet) override;

    void internalUnregisterListener(IEvents* listener);

private:
    concurrency::MpmcHashmap32<std::string, IEvents*> _iceListeners;
    concurrency::MpmcHashmap32<SocketAddress, IEvents*> _dtlsListeners;
    concurrency::MpmcHashmap32<__uint128_t, IEvents*> _iceResponseListeners;
};
} // namespace transport
