#pragma once
#include "concurrency/MpmcHashmap.h"
#include "test/integration/emulator/FakeEndpointImpl.h"
#include "test/transport/NetworkLink.h"
#include "transport/BaseUdpEndpoint.h"
#include "utils/SocketAddress.h"

namespace emulator
{
class FakeUdpEndpoint : public transport::UdpEndpoint, public FakeEndpointImpl
{
public:
    FakeUdpEndpoint(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const transport::SocketAddress& localPort,
        transport::RtcePoll& epoll,
        bool isShared,
        std::shared_ptr<fakenet::Gateway> gateway);

    virtual ~FakeUdpEndpoint();

    // ice::IceEndpoint
    void sendStunTo(const transport::SocketAddress& target,
        ice::Int96 transactionId,
        const void* data,
        size_t len,
        uint64_t timestamp) override;
    ice::TransportType getTransportType() const override;
    transport::SocketAddress getLocalPort() const override;
    void cancelStunTransaction(ice::Int96 transactionId) override;

    // transport::Endpoint
    void sendTo(const transport::SocketAddress& target, memory::UniquePacket packet) override;
    void registerListener(const std::string& stunUserName, IEvents* listener) override;
    void registerListener(const transport::SocketAddress& remotePort, IEvents* listener) override;
    void registerDefaultListener(IEvents* defaultListener) override;
    void unregisterListener(IEvents* listener) override;
    void unregisterListener(const transport::SocketAddress& remotePort, IEvents* listener) override;

    void start() override;
    void stop(IStopEvents* listener) override;
    bool configureBufferSizes(size_t sendBufferSize, size_t receiveBufferSize) override;

    const char* getName() const override;
    State getState() const override;

    // UdpEndpoint
    bool openPort(uint16_t port) override;
    bool isGood() const override;

    // NetworkNode
    void onReceive(fakenet::Protocol protocol,
        const transport::SocketAddress& source,
        const transport::SocketAddress& target,
        const void* data,
        size_t length,
        uint64_t timestamp) override;
    bool hasIp(const transport::SocketAddress& target) override;

    void process(uint64_t timestamp) override;
    std::shared_ptr<fakenet::NetworkLink> getDownlink() override { return _networkLink; }

    // Internal job interface.
    void internalUnregisterListener(IEvents* listener);
    void swapListener(const transport::SocketAddress& srcAddress, IEvents* newListener);
    // called on receiveJobs threads
    void internalReceive();
    void dispatchReceivedPacket(const transport::SocketAddress& srcAddress,
        memory::UniquePacket packet,
        uint64_t timestamp);

    EndpointMetrics getMetrics(uint64_t timestamp) const override
    {
        return _rateMetrics.toEndpointMetrics(_sendQueue.size());
    }

private:
    void internalUnregisterSourceListener(const transport::SocketAddress& remotePort, IEvents* listener);

private:
    logger::LoggableId _name;
    std::atomic<Endpoint::State> _state;
    transport::SocketAddress _localPort;
    concurrency::MpmcHashmap32<std::string, IEvents*> _iceListeners;
    concurrency::MpmcHashmap32<transport::SocketAddress, IEvents*> _dtlsListeners;
    concurrency::MpmcHashmap32<ice::Int96, IEvents*> _iceResponseListeners;

    jobmanager::JobQueue _receiveJobs;
    jobmanager::JobQueue _sendJobs;
    memory::PacketPoolAllocator& _allocator;
    memory::PacketPoolAllocator _networkLinkAllocator;
    concurrency::MpmcQueue<OutboundPacket> _sendQueue;
    concurrency::MpmcQueue<InboundPacket> _receiveQueue;

    std::atomic<IEvents*> _defaultListener;
    std::shared_ptr<fakenet::Gateway> _network;
    std::shared_ptr<fakenet::NetworkLink> _networkLink;

    std::atomic_flag _pendingRead = ATOMIC_FLAG_INIT;
};

} // namespace emulator
