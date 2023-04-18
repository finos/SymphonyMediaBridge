#pragma once
#include "concurrency/MpmcHashmap.h"
#include "jobmanager/JobQueue.h"
#include "test/integration/emulator/FakeEndpointImpl.h"
#include "test/transport/FakeNetwork.h"
#include "test/transport/NetworkLink.h"
#include "transport/BaseUdpEndpoint.h"
#include "transport/TcpEndpoint.h"
#include "utils/SocketAddress.h"

namespace emulator
{

class FakeTcpEndpoint : public transport::TcpEndpoint, public FakeEndpointImpl
{
public:
    FakeTcpEndpoint(jobmanager::JobManager& jobManager,
        memory::PacketPoolAllocator& allocator,
        const transport::SocketAddress& localPort,
        std::shared_ptr<fakenet::Gateway> gateway);

    virtual ~FakeTcpEndpoint();

    // ice::IceEndpoint
    void sendStunTo(const transport::SocketAddress& target,
        __uint128_t transactionId,
        const void* data,
        size_t len,
        uint64_t timestamp) override;
    ice::TransportType getTransportType() const override { return ice::TransportType::TCP; }
    transport::SocketAddress getLocalPort() const override { return _localPort; };
    void cancelStunTransaction(__uint128_t transactionId) override {}

    // transport::Endpoint
    void sendTo(const transport::SocketAddress& target, memory::UniquePacket packet) override;
    void registerListener(const std::string& stunUserName, IEvents* listener) override
    {
        registerDefaultListener(listener);
    }
    void registerListener(const transport::SocketAddress& remotePort, IEvents* listener) override
    {
        registerDefaultListener(listener);
    }
    void registerDefaultListener(IEvents* defaultListener) override;
    void unregisterListener(IEvents* listener) override;
    void unregisterListener(const transport::SocketAddress& remotePort, IEvents* listener) override
    {
        unregisterListener(listener);
    }

    void start() override {}
    void stop(IStopEvents* listener) override;
    bool configureBufferSizes(size_t sendBufferSize, size_t receiveBufferSize) override { return true; }

    const char* getName() const override { return _name.c_str(); }
    State getState() const override { return _state; }

    EndpointMetrics getMetrics(uint64_t timestamp) const override
    {
        return _rateMetrics.toEndpointMetrics(_sendQueue.size());
    }

    // NetworkNode
    void sendTo(const transport::SocketAddress& source,
        const transport::SocketAddress& target,
        const void* data,
        size_t length,
        uint64_t timestamp) override;
    bool hasIp(const transport::SocketAddress& target) override { return target == _localPort; }

    void process(uint64_t timestamp) override;
    std::shared_ptr<fakenet::NetworkLink> getDownlink() override { return _networkLink; }

    // called on receiveJobs threads
    void internalReceive();
    void dispatchReceivedPacket(const transport::SocketAddress& srcAddress,
        memory::UniquePacket packet,
        uint64_t timestamp);

private:
    void connect(const transport::SocketAddress& target);
    void internalUnregisterListener(IEvents* listener);

private:
    logger::LoggableId _name;
    std::atomic<Endpoint::State> _state;
    transport::SocketAddress _localPort;

    std::atomic<IEvents*> _defaultListener;
    memory::PacketPoolAllocator& _allocator;
    memory::PacketPoolAllocator _networkLinkAllocator;
    std::shared_ptr<fakenet::Gateway> _network;
    std::shared_ptr<fakenet::NetworkLink> _networkLink;
    concurrency::MpmcQueue<OutboundPacket> _sendQueue;
    concurrency::MpmcQueue<InboundPacket> _receiveQueue;
    jobmanager::JobQueue _receiveJobs;
    jobmanager::JobQueue _sendJobs;

    std::atomic_flag _pendingRead = ATOMIC_FLAG_INIT;
};

} // namespace emulator
