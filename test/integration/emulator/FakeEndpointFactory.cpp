#include "FakeEndpointFactory.h"
#include "FakeTcpServerEndpoint.h"
#include "FakeUdpEndpoint.h"
#include "transport/RecordingEndpoint.h"
#include <memory>

namespace emulator
{
class FakeRecordingEndpoint : public transport::RecordingEndpoint
{
public:
    FakeRecordingEndpoint() : _name("FakeRecordingEndpoint") {}
    virtual ~FakeRecordingEndpoint() {}

    void sendStunTo(const transport::SocketAddress& target,
        __uint128_t transactionId,
        const void* data,
        size_t len,
        uint64_t timestamp) override
    {
        assert(false);
    }

    void cancelStunTransaction(__uint128_t transactionId) override { assert(false); }

    void registerListener(const std::string& stunUserName, Endpoint::IEvents* listener) override { assert(false); };
    void registerListener(const transport::SocketAddress& remotePort, Endpoint::IEvents* listener) override
    {
        assert(false);
    };

    void unregisterListener(Endpoint::IEvents* listener) override { assert(false); };
    void unregisterListener(const transport::SocketAddress& remotePort, Endpoint::IEvents* listener) override
    {
        assert(false);
    }

    void registerRecordingListener(const transport::SocketAddress& remotePort, IRecordingEvents* listener) override {}

    void unregisterRecordingListener(IRecordingEvents* listener) override {}

    bool openPort(uint16_t port) override { return false; }
    bool isGood() const override { return true; }
    ice::TransportType getTransportType() const override { return ice::TransportType::UDP; }
    transport::SocketAddress getLocalPort() const override { return transport::SocketAddress(); }

    void sendTo(const transport::SocketAddress& target, memory::UniquePacket packet) override {}

    void registerDefaultListener(IEvents* defaultListener) override{};

    void start() override {}
    void stop(IStopEvents* listener) override {}

    bool configureBufferSizes(size_t sendBufferSize, size_t receiveBufferSize) override { return true; }

    const char* getName() const override { return _name.c_str(); }
    State getState() const override { return transport::Endpoint::State::CLOSED; }

    EndpointMetrics getMetrics(uint64_t timestamp) const override { return EndpointMetrics(); }

private:
    logger::LoggableId _name;
};

FakeEndpointFactory::FakeEndpointFactory(std::shared_ptr<fakenet::Gateway> network, EndpointCallback callback)
    : _network(network),
      _callback(callback)
{
}

transport::UdpEndpoint* FakeEndpointFactory::createUdpEndpoint(jobmanager::JobManager& jobManager,
    size_t maxSessionCount,
    memory::PacketPoolAllocator& allocator,
    const transport::SocketAddress& localPort,
    transport::RtcePoll& epoll,
    bool isShared)
{
    auto endpoint =
        new emulator::FakeUdpEndpoint(jobManager, maxSessionCount, allocator, localPort, epoll, isShared, _network);
    _network->addLocal(static_cast<fakenet::NetworkNode*>(endpoint));

    _callback(endpoint->getDownlink(), localPort, endpoint->getName());

    return endpoint;
}

transport::TcpEndpoint* FakeEndpointFactory::createTcpEndpoint(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    const transport::SocketAddress& localPort,
    transport::RtcePoll& epoll)
{
    auto endpoint = new FakeTcpEndpoint(jobManager, allocator, localPort, _network);
    _network->addLocal(endpoint);
    return endpoint;
}

transport::TcpEndpoint* FakeEndpointFactory::createTcpEndpoint(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    transport::RtcePoll& epoll,
    int fd,
    const transport::SocketAddress& localPort,
    const transport::SocketAddress& peerPort)
{
    return nullptr;
}

transport::ServerEndpoint* FakeEndpointFactory::createTcpServerEndpoint(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    transport::RtcePoll& epoll,
    uint32_t acceptBacklog,
    transport::TcpEndpointFactory* transportFactory,
    const transport::SocketAddress& localPort,
    const config::Config& config)
{
    auto serverEndpoint =
        new FakeTcpServerEndpoint(jobManager, allocator, acceptBacklog, transportFactory, localPort, config, _network);

    _network->addLocal(static_cast<fakenet::NetworkNode*>(serverEndpoint));

    _callback(serverEndpoint->getDownlink(), localPort, serverEndpoint->getName());

    return serverEndpoint;
}

transport::RecordingEndpoint* FakeEndpointFactory::createRecordingEndpoint(jobmanager::JobManager& jobManager,
    size_t maxSessionCount,
    memory::PacketPoolAllocator& allocator,
    const transport::SocketAddress& localPort,
    transport::RtcePoll& epoll,
    bool isShared)
{
    return new FakeRecordingEndpoint();
}

} // namespace emulator
