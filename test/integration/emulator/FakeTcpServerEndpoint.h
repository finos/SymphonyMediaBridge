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
        transport::EndpointFactory& endpointFactory,
        transport::RtcePoll& epoll);

    virtual const transport::SocketAddress getLocalPort() const override { return _localPort; };
    virtual void registerListener(const std::string& stunUserName, IEvents* listener) override;
    virtual void unregisterListener(const std::string& stunUserName, IEvents* listener) override;
    virtual bool isGood() const override { return true; }
    virtual void start() override { _state = transport::Endpoint::State::CONNECTED; };
    virtual void stop(transport::ServerEndpoint::IStopEvents* event) override;
    virtual const char* getName() const override { return _name.c_str(); }
    virtual transport::Endpoint::State getState() const override { return _state; }
    virtual void maintenance(uint64_t timestamp) override;

    virtual std::shared_ptr<fakenet::NetworkLink> getDownlink() override { return nullptr; }

private:
    // networkNode
    virtual void onReceive(fakenet::Protocol protocol,
        const transport::SocketAddress& source,
        const transport::SocketAddress& target,
        const void* data,
        size_t length,
        uint64_t timestamp) override;
    virtual bool hasIp(const transport::SocketAddress& target) override;
    virtual void process(uint64_t timestamp) override;

    void internalReceive();

    void internalUnregisterListener(const std::string& stunUserName, ServerEndpoint::IEvents* listener);

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

    std::atomic<IEvents*> _defaultListener;
    std::shared_ptr<fakenet::Gateway> _network;

    std::unordered_map<transport::SocketAddress, std::shared_ptr<FakeTcpEndpoint>> _endpoints;

    std::atomic_flag _pendingRead = ATOMIC_FLAG_INIT;
};
} // namespace emulator
