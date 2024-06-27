#include "test/integration/emulator/FakeTcpEndpoint.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "transport/dtls/SslDtls.h"
#include "utils/Function.h"

namespace emulator
{
std::atomic_uint16_t FakeTcpEndpoint::_portCounter(1000);
std::atomic_int FakeTcpEndpoint::_fdGenerator(1);

FakeTcpEndpoint::FakeTcpEndpoint(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    const transport::SocketAddress& nic,
    std::shared_ptr<fakenet::Gateway> gateway)
    : _name("FakeTcpEndpoint"),
      _state(State::CREATED),
      _localPort(nic), // could check that port does not exist
      _defaultListener(nullptr),
      _allocator(allocator),
      _networkLinkAllocator(8092, "networkLink"),
      _network(gateway),
      _networkLink(std::make_shared<fakenet::NetworkLink>(_name.c_str(), 1500000, 1950 * 1024, 3000)),
      _sendQueue(256 * 1024),
      _receiveQueue(256 * 1024),
      _receiveJobs(jobManager, 256 * 1024),
      _sendJobs(jobManager, 256 * 1024),
      _fakeFd(++_fdGenerator)
{
    while (!_network->isLocalPortFree(_localPort.setPort(_portCounter++))) {}
}

FakeTcpEndpoint::FakeTcpEndpoint(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    const transport::SocketAddress& localPort,
    const transport::SocketAddress& peerPort,
    std::shared_ptr<fakenet::Gateway> gateway)
    : _name("FakeTcpEndpoint"),
      _state(State::CONNECTING),
      _localPort(localPort),
      _peerPort(peerPort),
      _defaultListener(nullptr),
      _allocator(allocator),
      _networkLinkAllocator(8092, "networkLink"),
      _network(gateway),
      _networkLink(std::make_shared<fakenet::NetworkLink>(_name.c_str(), 1500000, 1950 * 1024, 3000)),
      _sendQueue(256 * 1024),
      _receiveQueue(256 * 1024),
      _receiveJobs(jobManager, 256 * 1024),
      _sendJobs(jobManager, 256 * 1024)
{
}

FakeTcpEndpoint::~FakeTcpEndpoint()
{
    _network->removeNode(this);
    logger::debug("deleting %s - %s", _name.c_str(), _localPort.toString().c_str(), _peerPort.toString().c_str());
    _localPort = transport::SocketAddress();
}

void FakeTcpEndpoint::sendStunTo(const transport::SocketAddress& target,
    __uint128_t transactionId,
    const void* data,
    size_t len,
    const uint64_t timestamp)
{
    auto* msg = ice::StunMessage::fromPtr(data);
    auto names = msg->getAttribute<ice::StunUserName>(ice::StunAttribute::USERNAME);
    if (names)
    {
        auto _localUser = names->getNames().second;
    }

    if (_state == State::CREATED)
    {
        connect(target);
    }

    auto packet = memory::makeUniquePacket(_networkLinkAllocator, data, len);
    if (packet)
    {
        if (_state != State::CONNECTED)
        {
            _pendingStun = std::move(packet);
            return;
        }
        else
        {
            sendTo(target, std::move(packet));
        }
    }
}

void FakeTcpEndpoint::connect(const transport::SocketAddress& target)
{
    _network->addLocal(this);
    _state = State::CONNECTING;
    _peerPort = target;

    if (!_sendQueue.push(
            {fakenet::Protocol::SYN, _localPort, target, memory::makeUniquePacket(_networkLinkAllocator, nullptr, 0)}))
    {
        logger::error("Can't send: send queue is full!", _name.c_str());
    }
}

void FakeTcpEndpoint::sendSynAck(const transport::SocketAddress& target)
{
    if (!_sendQueue.push({fakenet::Protocol::SYN_ACK,
            _localPort,
            target,
            memory::makeUniquePacket(_networkLinkAllocator, nullptr, 0)}))
    {
        logger::error("Can't send: send queue is full!", _name.c_str());
    }
}

void FakeTcpEndpoint::sendTo(const transport::SocketAddress& target, memory::UniquePacket uniquePacket)
{
    if (!uniquePacket)
    {
        return;
    }

    if (target.getFamily() != _localPort.getFamily())
    {
        logger::debug("incompatible target address", _name.c_str());
        return;
    }

    assert(!memory::PacketPoolAllocator::isCorrupt(uniquePacket.get()));
    assert(!isWeirdPacket(*uniquePacket));
    if (!_sendQueue.push({fakenet::Protocol::TCPDATA,
            _localPort,
            target,
            memory::makeUniquePacket(_networkLinkAllocator, *uniquePacket)}))
    {
        logger::error("Can't send: send queue is full!", _name.c_str());
    }
}

void FakeTcpEndpoint::onReceive(fakenet::Protocol protocol,
    const transport::SocketAddress& source,
    const transport::SocketAddress& target,
    const void* data,
    size_t length,
    uint64_t timestamp)
{
    assert(hasIp(target));
    assert(protocol != fakenet::Protocol::UDP);
    auto packet = memory::makeUniquePacket(_networkLinkAllocator, data, length);
    assert(!isWeirdPacket(*packet));
    _networkLink->push(serializeInbound(_networkLinkAllocator, protocol, source, data, length), timestamp);
}

void FakeTcpEndpoint::stop(Endpoint::IStopEvents* listener)
{
    if (_state == State::CONNECTING || _state == State::CONNECTED)
    {
        logger::info("stopping", _name.c_str());
        _state = State::STOPPING;
        _network->removeNode(this);
        // could await the queues to drain
        if (listener)
        {
            _receiveJobs.post([this, listener]() { listener->onEndpointStopped(this); });
        }
    }
    else if (_state == State::CREATED || _state == State::STOPPING)
    {
        if (listener)
        {
            _receiveJobs.post([this, listener]() { listener->onEndpointStopped(this); });
        }
    }
}

void FakeTcpEndpoint::registerDefaultListener(IEvents* listener)
{
    if (_defaultListener == listener)
    {
        return;
    }

    if (_defaultListener != nullptr)
    {
        unregisterListener(_defaultListener);
    }
    _defaultListener = listener;
    listener->onRegistered(*this);
}

void FakeTcpEndpoint::unregisterListener(IEvents* listener)
{
    if (!_receiveJobs.post(utils::bind(&FakeTcpEndpoint::internalUnregisterListener, this, listener)))
    {
        logger::error("failed to post unregister job", _name.c_str());
    }
}

void FakeTcpEndpoint::internalUnregisterListener(IEvents* listener)
{
    logger::debug("unregister %p", _name.c_str(), listener);
    if (listener == _defaultListener)
    {
        _defaultListener = nullptr;
        listener->onUnregistered(*this);
    }
}

void FakeTcpEndpoint::internalReceive()
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
            assert(!isWeirdPacket(*packetInfo.packet));
            dispatchReceivedPacket(packetInfo.protocol,
                packetInfo.source,
                memory::makeUniquePacket(_allocator, *packetInfo.packet),
                receiveTime);
        }
    }
}

void FakeTcpEndpoint::dispatchReceivedPacket(fakenet::Protocol protocol,
    const transport::SocketAddress& srcAddress,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    transport::Endpoint::IEvents* listener = _defaultListener;
    if (!listener)
    {
        logger::warn("No listener for packet from %s", _name.c_str(), srcAddress.toString().c_str());
        return;
    }

    if (packet->getLength() == 0 && protocol == fakenet::Protocol::SYN_ACK)
    {
        _state = State::CONNECTED;
        if (_pendingStun)
        {
            sendTo(_peerPort, std::move(_pendingStun));
        }
        return;
    }
    if (ice::isStunMessage(packet->get(), packet->getLength()))
    {
        listener->onIceReceived(*this, srcAddress, _localPort, std::move(packet), timestamp);
    }
    else if (transport::isDtlsPacket(packet->get(), packet->getLength()))
    {
        listener->onDtlsReceived(*this, srcAddress, _localPort, std::move(packet), timestamp);
    }
    else if (rtp::isRtcpPacket(packet->get(), packet->getLength()))
    {
        listener->onRtcpReceived(*this, srcAddress, _localPort, std::move(packet), timestamp);
    }
    else if (rtp::isRtpPacket(packet->get(), packet->getLength()))
    {
        listener->onRtpReceived(*this, srcAddress, _localPort, std::move(packet), timestamp);
    }
    else
    {
        logger::info("Unexpected packet from %s", _name.c_str(), srcAddress.toString().c_str());
    }
    // unexpected packet that can come from anywhere. We do not log as it facilitates DoS
}

void FakeTcpEndpoint::process(uint64_t timestamp)
{
    const auto start = utils::Time::getAbsoluteTime();

    if (_state != Endpoint::CONNECTED && _state != Endpoint::CONNECTING)
    {
        return;
    }

    // Retrieve those packets that are due to releasing after delay.
    for (auto packet = _networkLink->pop(timestamp); packet; packet = _networkLink->pop(timestamp))
    {
        auto inboundPacket = deserializeInbound(_networkLinkAllocator, std::move(packet));
        assert(!isWeirdPacket(*inboundPacket.packet));
        _receiveQueue.push(std::move(inboundPacket));

        if (!_pendingRead.test_and_set())
        {
            if (!_receiveJobs.post(utils::bind(&FakeTcpEndpoint::internalReceive, this)))
            {
                logger::warn("receive queue full", _name.c_str());
            }
        }
    }

    size_t byteCount = 0;
    for (OutboundPacket packetInfo; _sendQueue.pop(packetInfo);)
    {
        auto& packet = packetInfo.packet;
        byteCount += packet->getLength();

        assert(!isWeirdPacket(*packet));
        _network->onReceive(packetInfo.protocol,
            _localPort,
            packetInfo.targetAddress,
            packet->get(),
            packet->getLength(),
            start);
    }

    const auto sendTimestamp = utils::Time::getAbsoluteTime();
    _rateMetrics.sendTracker.update(byteCount, sendTimestamp);
}

} // namespace emulator
