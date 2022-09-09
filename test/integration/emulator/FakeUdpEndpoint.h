#pragma once
#include "concurrency/MpmcHashmap.h"
#include "test/transport/FakeNetwork.h"
#include "test/transport/NetworkLink.h"
#include "transport/BaseUdpEndpoint.h"
#include "utils/SocketAddress.h"

namespace emulator
{
class FakeUdpEndpoint : public transport::UdpEndpoint, public fakenet::NetworkNode
{
public:
    FakeUdpEndpoint(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const transport::SocketAddress& localPort,
        transport::RtcePoll& epoll,
        bool isShared,
        std::shared_ptr<fakenet::Gateway>);

    virtual ~FakeUdpEndpoint();

    // ice::IceEndpoint
    virtual void sendStunTo(const transport::SocketAddress& target,
        __uint128_t transactionId,
        const void* data,
        size_t len,
        uint64_t timestamp) override;
    virtual ice::TransportType getTransportType() const override;
    virtual transport::SocketAddress getLocalPort() const override;
    virtual void cancelStunTransaction(__uint128_t transactionId) override;

    // transport::Endpoint
    virtual void sendTo(const transport::SocketAddress& target, memory::UniquePacket packet) override;
    virtual void registerListener(const std::string& stunUserName, IEvents* listener) override;
    virtual void registerListener(const transport::SocketAddress& remotePort, IEvents* listener) override;
    virtual void registerDefaultListener(IEvents* defaultListener) override;
    virtual void unregisterListener(IEvents* listener) override;
    virtual void start() override;
    virtual void stop(IStopEvents* listener) override;
    virtual bool configureBufferSizes(size_t sendBufferSize, size_t receiveBufferSize) override;
    virtual bool isShared() const override;
    virtual const char* getName() const override;
    virtual State getState() const override;

    // transport::RtcePoll::IEventListener
    virtual void onSocketPollStarted(int fd) override;
    virtual void onSocketPollStopped(int fd) override;
    virtual void onSocketReadable(int fd) override;
    virtual void onSocketWriteable(int fd) override;
    virtual void onSocketShutdown(int fd) override;

    // UdpEndpoint
    virtual bool openPort(uint16_t port) override;
    virtual bool isGood() const override;
    virtual EndpointMetrics getMetrics(uint64_t timestamp) const override final;

    // NetworkNode
    virtual void sendTo(const transport::SocketAddress& source,
        const transport::SocketAddress& target,
        const void* data,
        size_t length,
        uint64_t timestamp) override;
    virtual bool hasIp(const transport::SocketAddress& target) override;
    virtual void process(uint64_t timestamp) override;
    virtual std::shared_ptr<fakenet::NetworkLink> getDownlink() override { return _networkLink; }

    // Internal job interface.
    void internalUnregisterListener(IEvents* listener);

    // called on receiveJobs threads
    void internalReceive();
    void dispatchReceivedPacket(const transport::SocketAddress& srcAddress, memory::UniquePacket packet);

private:
    struct InboundPacket
    {
        transport::SocketAddress address;
        memory::UniquePacket packet;
    };

    struct OutboundPacket
    {
        transport::SocketAddress address;
        memory::Packet packet;
    };

private:
    memory::UniquePacket serializeInbound(const transport::SocketAddress& source, const void* data, size_t length);
    InboundPacket deserializeInbound(memory::UniquePacket packet);

private:
    std::atomic<Endpoint::State> _state;
    logger::LoggableId _name;
    const bool _isShared;
    transport::SocketAddress _localPort;
    concurrency::MpmcHashmap32<std::string, IEvents*> _iceListeners;
    concurrency::MpmcHashmap32<transport::SocketAddress, IEvents*> _dtlsListeners;
    concurrency::MpmcHashmap32<__uint128_t, IEvents*> _iceResponseListeners;

    jobmanager::JobQueue _receiveJobs;
    jobmanager::JobQueue _sendJobs;
    memory::PacketPoolAllocator& _allocator;
    concurrency::MpmcQueue<OutboundPacket> _sendQueue;
    concurrency::MpmcQueue<InboundPacket> _receiveQueue;

    std::atomic<IEvents*> _defaultListener;
    std::shared_ptr<fakenet::Gateway> _network;
    memory::PacketPoolAllocator _networkLinkAllocator;
    std::shared_ptr<fakenet::NetworkLink> _networkLink;

    struct RateMetrics
    {
        utils::TrackerWithSnapshot<10, utils::Time::ms * 100, utils::Time::sec> receiveTracker;
        utils::TrackerWithSnapshot<10, utils::Time::ms * 100, utils::Time::sec> sendTracker;
        EndpointMetrics toEndpointMetrics(size_t queueSize) const
        {
            return EndpointMetrics(queueSize,
                receiveTracker.snapshot.load() * 8 * utils::Time::ms,
                sendTracker.snapshot.load() * 8 * utils::Time::ms);
        }
    } _rateMetrics;

    std::atomic_flag _pendingRead = ATOMIC_FLAG_INIT;
};

} // namespace emulator