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
    std::shared_ptr<fakenet::Gateway> gateway,
    transport::EndpointFactory& endpointFactory,
    transport::RtcePoll& epoll)
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
      _transportFactory(transportFactory)
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

void FakeTcpServerEndpoint::onReceive(fakenet::Protocol protocol,
    const transport::SocketAddress& source,
    const transport::SocketAddress& target,
    const void* data,
    size_t length,
    uint64_t timestamp)
{
    assert(hasIp(target));
    // assert(protocol != fakenet::Protocol::UDP);

    auto packet = memory::makeUniquePacket(_networkLinkAllocator, data, length);
    assert(!isWeirdPacket(*packet));
    _receiveQueue.push(InboundPacket{protocol, source, std::move(packet)});
    if (!_pendingRead.test_and_set())
    {
        if (!_receiveJobs.post(utils::bind(&FakeTcpServerEndpoint::internalReceive, this)))
        {
            logger::warn("receive queue full", _name.c_str());
        }
    }
}

bool FakeTcpServerEndpoint::hasIp(const transport::SocketAddress& target)
{
    return (target == _localPort);
}

void FakeTcpServerEndpoint::process(uint64_t timestamp)
{
    const auto start = utils::Time::getAbsoluteTime();
    uint32_t packetCounter = 0;

    size_t byteCount = 0;
    for (OutboundPacket packetInfo; _sendQueue.pop(packetInfo);)
    {
        ++packetCounter;
        auto& packet = packetInfo.packet;
        byteCount += packet->getLength();

        _network->onReceive(packetInfo.protocol,
            packetInfo.sourceAddress,
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
            if (packetInfo.protocol == fakenet::Protocol::SYN)
            {
                // create new TcpEndpoint if it is also on a registered stunName
                auto tcpEndpoint = std::shared_ptr<FakeTcpEndpoint>(new FakeTcpEndpoint(_receiveJobs.getJobManager(),
                    _allocator,
                    _localPort,
                    packetInfo.source,
                    _network));
                // TODO must pass  Deleter here otherwise this cannot work. Since TransportFactoryImpl is not within our
                // scope we should refactor so EndpointFactory has the deleter and TransportFactory can wait for
                // endpoint factory to empty its garbage queue at termination

                _pendingTcpConnections[packetInfo.source] = tcpEndpoint;
                _network->addLocal(tcpEndpoint.get());
                tcpEndpoint->sendSynAck(packetInfo.source);
            }
            else if (packetInfo.protocol == fakenet::Protocol::TCPDATA)
            {
                auto it = _endpoints.find(packetInfo.source);
                if (it != _endpoints.end())
                {
                    auto endpoint = it->second.lock();
                    if (endpoint)
                    {
                        endpoint->onReceive(packetInfo.protocol,
                            packetInfo.source,
                            _localPort,
                            packetInfo.packet->get(),
                            packetInfo.packet->getLength(),
                            receiveTime);
                    }
                    else
                    {
                        _endpoints.erase(it);
                    }
                    continue;
                }

                auto pendingIt = _pendingTcpConnections.find(packetInfo.source);
                if (pendingIt != _pendingTcpConnections.end())
                {
                    auto tcpEndpoint = pendingIt->second;
                    if (tcpEndpoint->getState() == transport::Endpoint::State::CONNECTING)
                    {
                        if (ice::isStunMessage(packetInfo.packet->get(), packetInfo.packet->getLength()))
                        {
                            auto msg = ice::StunMessage::fromPtr(packetInfo.packet->get());
                            if (msg->header.isRequest())
                            {
                                auto users = msg->getAttribute<ice::StunUserName>(ice::StunAttribute::USERNAME);
                                if (users)
                                {
                                    const auto names = users->getNames();
                                    auto listenIt = _iceListeners.find(names.first);
                                    if (listenIt != _iceListeners.end())
                                    {
                                        tcpEndpoint->onFirstStun();
                                        _endpoints.emplace(pendingIt->first, tcpEndpoint);
                                        _pendingTcpConnections.erase(pendingIt);
                                        listenIt->second->onIceTcpConnect(tcpEndpoint,
                                            packetInfo.source,
                                            _localPort,
                                            std::move(packetInfo.packet),
                                            receiveTime);
                                        continue;
                                    }
                                }
                            }
                        }
                        else
                        {
                            _endpoints.erase(packetInfo.source);
                        }
                    }
                }
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
