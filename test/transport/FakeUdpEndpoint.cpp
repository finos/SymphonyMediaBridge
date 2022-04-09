#include "FakeUdpEndpoint.h"

namespace fakenet
{
FakeUdpEndpoint::FakeUdpEndpoint(const transport::SocketAddress& port,
    uint32_t uplinkBandwidthKbps,
    uint32_t downlinkBandwidth,
    NetworkNode* netSwitch)
    : _upLink(uplinkBandwidthKbps, 65000, 1480),
      _downLink(downlinkBandwidth, 65000, 1480)
{
}

void FakeUdpEndpoint::sendTo(const transport::SocketAddress& source,
    const transport::SocketAddress& target,
    const void* data,
    size_t length,
    const uint64_t timestamp)
{
}

bool FakeUdpEndpoint::hasIp(const transport::SocketAddress& target)
{
    return _localPort == target;
}

void FakeUdpEndpoint::process(uint64_t timestamp) {}

void FakeUdpEndpoint::sendTo(const transport::SocketAddress& target, memory::UniquePacket packet) {}

void FakeUdpEndpoint::registerListener(const std::string& stunUserName, IEvents* listener) {}
void FakeUdpEndpoint::registerListener(const transport::SocketAddress& remotePort, IEvents* listener) {}
void FakeUdpEndpoint::registerDefaultListener(IEvents* defaultListener) {}

void FakeUdpEndpoint::unregisterListener(IEvents* listener) {}

void FakeUdpEndpoint::start() {}
void FakeUdpEndpoint::closePort() {}

transport::SocketAddress FakeUdpEndpoint::getLocalPort() const
{
    return _localPort;
}

bool FakeUdpEndpoint::configureBufferSizes(size_t sendBufferSize, size_t receiveBufferSize)
{
    return true;
}
} // namespace fakenet
