#pragma once
#include "test/transport/FakeNetwork.h"
#include "test/transport/NetworkLink.h"
#include "transport/Endpoint.h"

namespace fakenet
{

class FakeUdpEndpoint : public transport::Endpoint, public NetworkNode
{
public:
    FakeUdpEndpoint(const transport::SocketAddress& port,
        uint32_t uplinkBandwidthKbps,
        uint32_t downlinkBandwidth,
        NetworkNode* netSwitch);

    void sendTo(const transport::SocketAddress& source,
        const transport::SocketAddress& target,
        const void* data,
        size_t length,
        uint64_t timestamp) override;
    bool hasIp(const transport::SocketAddress& target) override;
    void process(uint64_t timestamp) override;

    void sendTo(const transport::SocketAddress& target, memory::PacketPtr packet) override;

    void registerListener(const std::string& stunUserName, IEvents* listener) override;
    void registerListener(const transport::SocketAddress& remotePort, IEvents* listener) override;
    void registerDefaultListener(IEvents* defaultListener) override;

    void unregisterListener(IEvents* listener) override;

    void start() override;
    void closePort() override;

    transport::SocketAddress getLocalPort() const override;

    bool configureBufferSizes(size_t sendBufferSize, size_t receiveBufferSize) override;

private:
    transport::SocketAddress _localPort;

    NetworkLink _upLink;
    NetworkLink _downLink;
};
} // namespace fakenet
