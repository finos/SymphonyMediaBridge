#pragma once
#include "concurrency/MpmcHashmap.h"
#include "config/Config.h"
#include "jobmanager/JobManager.h"
#include "jobmanager/JobQueue.h"
#include "test/integration/emulator/FakeEndpointImpl.h"
#include "test/integration/emulator/FakeTcpEndpoint.h"
#include "test/transport/FakeNetwork.h"
#include "transport/Endpoint.h"
#include "transport/EndpointFactory.h"
#include "utils/SocketAddress.h"

namespace emulator
{
class FakeEndpointFactory;
class FakeTcpServerEndpoint : public transport::ServerEndpoint, public FakeEndpointImpl
{
public:
    FakeTcpServerEndpoint(jobmanager::JobManager& jobManager,
        memory::PacketPoolAllocator& allocator,
        uint32_t acceptBacklog,
        transport::TcpEndpointFactory* transportFactory,
        const transport::SocketAddress& localPort,
        const config::Config& config,
        std::shared_ptr<fakenet::Gateway> gateway,
        FakeEndpointFactory& endpointFactory,
        transport::RtcePoll& epoll);

    const transport::SocketAddress getLocalPort() const override { return _localPort; };
    void registerListener(const std::string& stunUserName, IEvents* listener) override;
    void unregisterListener(const std::string& stunUserName, IEvents* listener) override;
    bool isGood() const override { return true; }
    void start() override { _state = transport::Endpoint::State::CONNECTED; };
    void stop(transport::ServerEndpoint::IStopEvents* event) override;
    const char* getName() const override { return _name.c_str(); }
    transport::Endpoint::State getState() const override { return _state; }
    void maintenance(uint64_t timestamp) override;

    std::shared_ptr<fakenet::NetworkLink> getDownlink() override { return nullptr; }

private:
    // networkNode
    void onReceive(fakenet::Protocol protocol,
        const transport::SocketAddress& source,
        const transport::SocketAddress& target,
        const void* data,
        size_t length,
        uint64_t timestamp) override;
    bool hasIp(const transport::SocketAddress& target, fakenet::Protocol protocol) const override;
    bool hasIpClash(const NetworkNode& node) const override { return node.hasIp(_localPort, fakenet::Protocol::SYN); }
    fakenet::Protocol getProtocol() const override { return fakenet::Protocol::SYN; }
    void process(uint64_t timestamp) override;

    void internalReceive();

    void internalUnregisterListener(const std::string& stunUserName, ServerEndpoint::IEvents* listener);

    struct TcpEndpointItem
    {
        std::shared_ptr<transport::Endpoint> strongEndpoint;
        std::weak_ptr<transport::Endpoint> endpoint;
        FakeTcpEndpoint* tcpEndpoint;
    };

private:
    // map of FakeTcpEndpoints mapped on srcip:port
    // this allows us to route incoming packets onto the right endpint if it exists.

    logger::LoggableId _name;
    std::atomic<transport::Endpoint::State> _state;
    transport::SocketAddress _localPort;
    concurrency::MpmcHashmap32<std::string, IEvents*> _iceListeners;

    jobmanager::JobQueue _receiveJobs;
    jobmanager::JobQueue _sendJobs;
    memory::PacketPoolAllocator& _allocator;
    memory::PacketPoolAllocator _networkLinkAllocator;
    concurrency::MpmcQueue<OutboundPacket> _sendQueue;
    concurrency::MpmcQueue<InboundPacket> _receiveQueue;

    std::shared_ptr<fakenet::Gateway> _network;

    std::unordered_map<transport::SocketAddress, TcpEndpointItem> _endpoints;
    transport::TcpEndpointFactory* _transportFactory;
    FakeEndpointFactory& _endpointFactory;

    std::atomic_flag _pendingRead = ATOMIC_FLAG_INIT;
};
} // namespace emulator
