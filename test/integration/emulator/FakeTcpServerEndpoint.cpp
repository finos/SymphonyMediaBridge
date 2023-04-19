#include "test/integration/emulator/FakeTcpServerEndpoint.h"
#include "utils/Function.h"

namespace emulator
{

FakeTcpServerEndpoint::FakeTcpServerEndpoint(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    uint32_t acceptBacklog,
    transport::TcpEndpointFactory* transportFactory,
    const transport::SocketAddress& localPort,
    const config::Config& config,
    std::shared_ptr<fakenet::Gateway> gateway)
    : _name("FakeTcpServerEndpoint"),
      _state(transport::Endpoint::State::CREATED),
      _localPort(localPort),
      _iceListeners(4096),
      _receiveJobs(jobManager, 1024 * 256),
      _sendJobs(jobManager, 1024 * 256),
      _allocator(allocator),
      _networkLinkAllocator(8092, "networkLinkTcp"),
      _sendQueue(1024 * 256),
      _receiveQueue(1024 * 256),
      _network(gateway),
      _networkLink(std::make_shared<fakenet::NetworkLink>(_name.c_str(), 1500000, 1950 * 1024, 3000))
{
    if (!_network->isLocalPortFree(_localPort))
    {
        logger::error("TCP port already in use", _name.c_str());
    }
    else
    {
        _network->addLocal(this);
    }
}

void FakeTcpServerEndpoint::onReceive(const transport::SocketAddress& source,
    const transport::SocketAddress& target,
    const void* data,
    size_t length,
    uint64_t timestamp)
{
    assert(hasIp(target));
    _networkLink->push(serializeInbound(_networkLinkAllocator, source, data, length), timestamp);
}

bool FakeTcpServerEndpoint::hasIp(const transport::SocketAddress& target)
{
    return (target == _localPort);
}

void FakeTcpServerEndpoint::process(uint64_t timestamp)
{
    const auto start = utils::Time::getAbsoluteTime();
    uint32_t packetCounter = 0;

    // Retrieve those packets that are due to releasing after delay.
    for (auto packet = _networkLink->pop(timestamp); packet; packet = _networkLink->pop(timestamp))
    {
        _receiveQueue.push(deserializeInbound(_networkLinkAllocator, std::move(packet)));

        if (!_pendingRead.test_and_set())
        {
            if (!_receiveJobs.post(utils::bind(&FakeTcpServerEndpoint::internalReceive, this)))
            {
                logger::warn("receive queue full", _name.c_str());
            }
        }
    }

    size_t byteCount = 0;
    for (OutboundPacket packetInfo; _sendQueue.pop(packetInfo);)
    {
        ++packetCounter;
        auto& packet = packetInfo.packet;
        byteCount += packet->getLength();

        _network->onReceive(packetInfo.sourceAddress,
            packetInfo.targetAddress,
            packet->get(),
            packet->getLength(),
            start);
    }

    const auto sendTimestamp = utils::Time::getAbsoluteTime();
    _rateMetrics.sendTracker.update(byteCount, sendTimestamp);
}

void FakeTcpServerEndpoint::internalReceive()
{
    _pendingRead.clear(); // one extra job may be added after us
    const auto packetCount = _receiveQueue.size();
    if (packetCount <= 0)
    {
        return;
    }
    const auto receiveTime = utils::Time::getAbsoluteTime();
    for (unsigned long i = 0; i < packetCount; ++i)
    {
        InboundPacket packetInfo;
        if (_receiveQueue.pop(packetInfo) && packetInfo.packet)
        {
            _rateMetrics.receiveTracker.update(packetInfo.packet->getLength(), receiveTime);
            if (packetInfo.protocol == ProtocolIndicator::SYN)
            {
                // create new TcpEndpoint if it is also on a registered stunName
            }
            else if (packetInfo.protocol == ProtocolIndicator::TCPDATA)
            {
                // find tcp endpoint and dispatch on it
                /*dispatchReceivedPacket(packetInfo.address,
                    memory::makeUniquePacket(_allocator, *packetInfo.packet),
                    receiveTime);*/
            }
        }
    }
}

void FakeTcpServerEndpoint::registerListener(const std::string& stunUserName, IEvents* listener)
{
    if (_iceListeners.contains(stunUserName))
    {
        return;
    }

    logger::debug("register ICE listener for %s", _name.c_str(), stunUserName.c_str());
    _iceListeners.emplace(stunUserName, listener);
    listener->onServerPortRegistered(*this);
}

void FakeTcpServerEndpoint::unregisterListener(const std::string& stunUserName, ServerEndpoint::IEvents* listener)
{
    _receiveJobs.post(utils::bind(&FakeTcpServerEndpoint::internalUnregisterListener, this, stunUserName, listener));
}

void FakeTcpServerEndpoint::internalUnregisterListener(const std::string& stunUserName,
    ServerEndpoint::IEvents* listener)
{
    if (!_iceListeners.contains(stunUserName))
    {
        return;
    }

    _iceListeners.erase(stunUserName);
    listener->onServerPortUnregistered(*this);
}

void FakeTcpServerEndpoint::stop(ServerEndpoint::IStopEvents* listener)
{
    if (_state == transport::Endpoint::State::CONNECTING || _state == transport::Endpoint::State::CONNECTED)
    {
        _state = transport::Endpoint::State::STOPPING;

        _sendQueue.clear();
        _receiveQueue.clear();

        // Would be closing epoll subscription in a job and call a stop callback...
        _state = transport::Endpoint::State::CREATED;

        if (listener)
        {
            listener->onEndpointStopped(this);
        }
    }
}

void FakeTcpServerEndpoint::maintenance(uint64_t timestamp) {}

} // namespace emulator
