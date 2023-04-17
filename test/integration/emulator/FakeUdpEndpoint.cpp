#include "FakeUdpEndpoint.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "transport/EndpointFactoryImpl.h"
#include "transport/UdpEndpoint.h"
#include "transport/dtls/SslDtls.h"
#include "utils/Function.h"
#include "utils/SocketAddress.h"

namespace emulator
{
FakeUdpEndpoint::FakeUdpEndpoint(jobmanager::JobManager& jobManager,
    size_t maxSessionCount,
    memory::PacketPoolAllocator& allocator,
    const transport::SocketAddress& localPort,
    transport::RtcePoll& epoll,
    bool isShared,
    std::shared_ptr<fakenet::Gateway> network)
    : _state(CLOSED),
      _name("FakeUdpEndpoint"),
      _isShared(isShared),
      _localPort(localPort),
      _iceListeners(maxSessionCount * 2),
      _dtlsListeners(maxSessionCount * 16),
      _iceResponseListeners(maxSessionCount * 64),
      _receiveJobs(jobManager, maxSessionCount),
      _sendJobs(jobManager, 16),
      _allocator(allocator),
      _networkLinkAllocator(8092, "networkLink"),
      _sendQueue(maxSessionCount * 256),
      _receiveQueue(maxSessionCount * 256),
      _defaultListener(nullptr),
      _network(network),
      _networkLink(std::make_shared<fakenet::NetworkLink>(_name.c_str(), 1500000, 1950 * 1024, 3000))
{
    openPort(_localPort.getPort());
    _pendingRead.clear();
    _networkLink->setStaticDelay(0);
}

FakeUdpEndpoint::~FakeUdpEndpoint()
{
    logger::warn("~FakeUdpEndpoint, pending packets in the network link %zu", _name.c_str(), _networkLink->count());
}

// ice::IceEndpoint
void FakeUdpEndpoint::sendStunTo(const transport::SocketAddress& target,
    __uint128_t transactionId,
    const void* data,
    size_t len,
    uint64_t timestamp)
{
    auto* msg = ice::StunMessage::fromPtr(data);
    if (msg->header.isRequest() && !_iceResponseListeners.contains(transactionId))
    {
        auto names = msg->getAttribute<ice::StunUserName>(ice::StunAttribute::USERNAME);
        if (names)
        {
            auto localUser = names->getNames().second;
            auto it = _iceListeners.find(localUser);
            if (it != _iceListeners.cend())
            {
                assert(it->second);
                auto pair = _iceResponseListeners.emplace(transactionId, it->second);
                if (!pair.second)
                {
                    logger::warn("Pending ICE request lookup table is full", _name.c_str());
                }
                else
                {
                    const IndexableInteger<__uint128_t, uint32_t> id(transactionId);
                    logger::debug("register ICE listener for %04x%04x%04x", _name.c_str(), id[1], id[2], id[3]);
                }
            }
        }
    }

    sendTo(target, memory::makeUniquePacket(_allocator, data, len));
}

ice::TransportType FakeUdpEndpoint::getTransportType() const
{
    return ice::TransportType::UDP;
}

transport::SocketAddress FakeUdpEndpoint::getLocalPort() const
{
    return _localPort;
}

void FakeUdpEndpoint::cancelStunTransaction(__uint128_t transactionId)
{
    const bool posted = _receiveJobs.post([this, transactionId]() { _iceResponseListeners.erase(transactionId); });
    if (!posted)
    {
        logger::warn("failed to post unregister STUN transaction job", _name.c_str());
    }
}

// transport::Endpoint
void FakeUdpEndpoint::sendTo(const transport::SocketAddress& target, memory::UniquePacket uniquePacket)
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
    if (!_sendQueue.push({target, memory::makeUniquePacket(_networkLinkAllocator, *uniquePacket)}))
    {
        logger::error("Can't send: send queue is full!", _name.c_str());
    }
}

void FakeUdpEndpoint::registerListener(const std::string& stunUserName, IEvents* listener)
{
    if (_iceListeners.contains(stunUserName))
    {
        return;
    }

    logger::debug("register ICE listener for %s", _name.c_str(), stunUserName.c_str());
    _iceListeners.emplace(stunUserName, listener);
    listener->onRegistered(*this);
}

void FakeUdpEndpoint::registerListener(const transport::SocketAddress& srcAddress, IEvents* listener)
{
    auto it = _dtlsListeners.emplace(srcAddress, listener);
    if (it.second)
    {
        logger::debug("register listener for %s", _name.c_str(), srcAddress.toString().c_str());
        listener->onRegistered(*this);
    }
    else if (it.first != _dtlsListeners.cend() && it.first->second == listener)
    {
        // already registered
    }
    else
    {
        _receiveJobs.post(utils::bind(&FakeUdpEndpoint::swapListener, this, srcAddress, listener));
    }
}

void FakeUdpEndpoint::swapListener(const transport::SocketAddress& srcAddress, IEvents* newListener)
{
    auto it = _dtlsListeners.find(srcAddress);
    if (it != _dtlsListeners.cend())
    {
        // src port is re-used. Unregister will look at listener pointer
        if (it->second == newListener)
        {
            return;
        }

        if (it->second)
        {
            it->second->onUnregistered(*this);
        }
        it->second = newListener;
        newListener->onRegistered(*this);
        return;
    }

    logger::warn("dtls listener swap on %s skipped. Already removed", _name.c_str(), srcAddress.toString().c_str());
}

void FakeUdpEndpoint::registerDefaultListener(IEvents* defaultListener)
{
    _defaultListener = defaultListener;
}

void FakeUdpEndpoint::unregisterListener(IEvents* listener)
{
    if (!_receiveJobs.post(utils::bind(&FakeUdpEndpoint::internalUnregisterListener, this, listener)))
    {
        logger::error("failed to post unregister job", _name.c_str());
    }
}

void FakeUdpEndpoint::unregisterListener(const transport::SocketAddress& remotePort, IEvents* listener)
{
    if (!_receiveJobs.post(utils::bind(&FakeUdpEndpoint::internalUnregisterSourceListener, this, remotePort, listener)))
    {
        logger::error("failed to post unregister job", _name.c_str());
    }
}

void FakeUdpEndpoint::start()
{
    if (_state == CREATED)
    {
        _state = CONNECTING;
        _state = CONNECTED;
    }
}

void FakeUdpEndpoint::stop(IStopEvents* listener)
{
    if (_state == CONNECTING || _state == CONNECTED)
    {
        _state = STOPPING;

        _sendQueue.clear();
        _receiveQueue.clear();

        // Would be closing epoll subscription in a job and call a stop callback...
        _state = CREATED;

        if (listener)
        {
            listener->onEndpointStopped(this);
        }
    }
}

bool FakeUdpEndpoint::configureBufferSizes(size_t sendBufferSize, size_t receiveBufferSize)
{
    return true;
}

bool FakeUdpEndpoint::isShared() const
{
    return _isShared;
}

const char* FakeUdpEndpoint::getName() const
{
    return _name.c_str();
}

transport::Endpoint::State FakeUdpEndpoint::getState() const
{
    return _state;
}

bool FakeUdpEndpoint::openPort(uint16_t port)
{
    auto wantedAddress = _localPort;
    wantedAddress.setPort(port);
    if (!_network->isLocalPortFree(wantedAddress))
    {
        return false;
    }

    _localPort.setPort(port);
    _state = Endpoint::State::CREATED;
    return true;
}

bool FakeUdpEndpoint::isGood() const
{
    return _state != State::CLOSED;
};

EndpointMetrics FakeUdpEndpoint::getMetrics(uint64_t timestamp) const
{
    return _rateMetrics.toEndpointMetrics(_sendQueue.size());
}

void FakeUdpEndpoint::internalUnregisterListener(IEvents* listener)
{
    // Hashmap allows erasing elements while iterating.
    logger::debug("unregister %p", _name.c_str(), listener);
    for (auto& item : _iceListeners)
    {
        if (item.second == listener)
        {
            _iceListeners.erase(item.first);
            listener->onUnregistered(*this);
            break;
        }
    }

    for (auto& responseListener : _iceResponseListeners)
    {
        if (responseListener.second == listener)
        {
            _iceResponseListeners.erase(responseListener.first);
            // must be iceListener to be iceResponseListener so no extra unreg notification
        }
    }

    for (auto& item : _dtlsListeners)
    {
        if (item.second == listener)
        {
            _dtlsListeners.erase(item.first);
            listener->onUnregistered(*this);
        }
    }
}

void FakeUdpEndpoint::internalUnregisterSourceListener(const transport::SocketAddress& remotePort, IEvents* listener)
{
    // Hashmap allows erasing elements while iterating.
    logger::debug("unregister %p", _name.c_str(), listener);

    auto it = _dtlsListeners.find(remotePort);
    if (it == _dtlsListeners.end())
    {
        return;
    }

    for (auto& item : _dtlsListeners)
    {
        if (item.second == listener)
        {
            logger::debug("remove listener on %s, unlisten %s",
                _name.c_str(),
                remotePort.toString().c_str(),
                item.first.toString().c_str());
            _dtlsListeners.erase(item.first);
            listener->onUnregistered(*this);
            break;
        }
    }
}

memory::UniquePacket FakeUdpEndpoint::serializeInbound(const transport::SocketAddress& source,
    const void* data,
    size_t length)
{
    memory::FixedPacket<2000> packet;
    auto ip = source.ipToString();
    packet.append(ip.c_str(), ip.length() + 1);
    uint16_t port = source.getPort();
    packet.append((void*)&port, sizeof(uint16_t));
    packet.append(data, length);

    return memory::makeUniquePacket(_networkLinkAllocator, packet.get(), packet.getLength());
}

FakeUdpEndpoint::InboundPacket FakeUdpEndpoint::deserializeInbound(memory::UniquePacket packet)
{
    const auto ipLen = strlen((char*)packet->get()) + 1;
    const auto prefixLength = ipLen + sizeof(uint16_t);
    const auto dataLength = packet->getLength() - prefixLength;
    const auto ip = std::string((char*)packet->get());
    const int* const port = (int*)(packet->get() + ipLen);

    const auto source = transport::SocketAddress::parse(ip, *port);

    return {source, memory::makeUniquePacket(_networkLinkAllocator, packet->get() + prefixLength, dataLength)};
}

void FakeUdpEndpoint::sendTo(const transport::SocketAddress& source,
    const transport::SocketAddress& target,
    const void* data,
    size_t length,
    uint64_t timestamp)
{
    assert(hasIp(target));
    _networkLink->push(serializeInbound(source, data, length), timestamp);
}

bool FakeUdpEndpoint::hasIp(const transport::SocketAddress& target)
{
    if (_state != State::CONNECTED)
    {
        return false;
    }
    return target == _localPort;
}

void FakeUdpEndpoint::process(uint64_t timestamp)
{
    const auto start = utils::Time::getAbsoluteTime();
    uint32_t packetCounter = 0;

    if (_state != Endpoint::CONNECTED)
    {
        return;
    }

    // Retrieve those packets that are due to releasing after delay.
    for (auto packet = _networkLink->pop(timestamp); packet; packet = _networkLink->pop(timestamp))
    {
        _receiveQueue.push(deserializeInbound(std::move(packet)));

        if (!_pendingRead.test_and_set())
        {
            if (!_receiveJobs.post(utils::bind(&FakeUdpEndpoint::internalReceive, this)))
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

        _network->sendTo(_localPort, packetInfo.address, packet->get(), packet->getLength(), start);
    }

    const auto sendTimestamp = utils::Time::getAbsoluteTime();
    _rateMetrics.sendTracker.update(byteCount, sendTimestamp);
}

void FakeUdpEndpoint::internalReceive()
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
            dispatchReceivedPacket(packetInfo.address,
                memory::makeUniquePacket(_allocator, *packetInfo.packet),
                receiveTime);
        }
    }
}

void FakeUdpEndpoint::dispatchReceivedPacket(const transport::SocketAddress& srcAddress,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    transport::UdpEndpoint::IEvents* listener = _defaultListener;

    if (ice::isStunMessage(packet->get(), packet->getLength()))
    {
        auto msg = ice::StunMessage::fromPtr(packet->get());

        if (msg->header.isRequest())
        {
            auto users = msg->getAttribute<ice::StunUserName>(ice::StunAttribute::USERNAME);
            if (users)
            {
                auto userName = users->getNames().first;
                listener = _iceListeners.getItem(userName);
            }
            logger::debug("ICE request for %s from %s",
                _name.c_str(),
                users->getNames().first.c_str(),
                srcAddress.toString().c_str());
        }
        else if (msg->header.isResponse())
        {
            auto transactionId = msg->header.transactionId.get();
            listener = _iceResponseListeners.getItem(transactionId);
            if (listener)
            {
                const IndexableInteger<__uint128_t, uint32_t> id(transactionId);
                logger::debug("STUN response received for transaction %04x%04x%04x",
                    _name.c_str(),
                    id[1],
                    id[2],
                    id[3]);
                _iceResponseListeners.erase(transactionId);
            }
        }

        if (listener)
        {
            listener->onIceReceived(*this, srcAddress, _localPort, std::move(packet), timestamp);
            return;
        }
        else
        {
            logger::debug("cannot find listener for STUN", _name.c_str());
        }
    }
    else if (transport::isDtlsPacket(packet->get()))
    {
        listener = _dtlsListeners.getItem(srcAddress);
        listener = listener ? listener : _defaultListener.load();
        if (listener)
        {
            listener->onDtlsReceived(*this, srcAddress, _localPort, std::move(packet), timestamp);
            return;
        }
        else
        {
            logger::debug("cannot find listener for DTLS source %s", _name.c_str(), srcAddress.toString().c_str());
        }
    }
    else if (rtp::isRtcpPacket(packet->get(), packet->getLength()))
    {
        auto rtcpReport = rtp::RtcpReport::fromPtr(packet->get(), packet->getLength());
        if (rtcpReport)
        {
            listener = _dtlsListeners.getItem(srcAddress);

            if (listener)
            {
                listener->onRtcpReceived(*this, srcAddress, _localPort, std::move(packet), timestamp);
                return;
            }
            else
            {
                logger::debug("cannot find listener for RTCP", _name.c_str());
            }
        }
    }
    else if (rtp::isRtpPacket(packet->get(), packet->getLength()))
    {
        auto rtpPacket = rtp::RtpHeader::fromPacket(*packet);
        if (rtpPacket)
        {
            listener = _dtlsListeners.getItem(srcAddress);

            if (listener)
            {
                listener->onRtpReceived(*this, srcAddress, _localPort, std::move(packet), timestamp);
                return;
            }
            else
            {
                logger::debug("cannot find listener for RTP", _name.c_str());
            }
        }
    }
    else
    {
        logger::info("Unexpected packet from %s", _name.c_str(), srcAddress.toString().c_str());
    }
    // unexpected packet that can come from anywhere. We do not log as it facilitates DoS
}

void FakeUdpEndpoint::onSocketPollStarted(int fd) {}
void FakeUdpEndpoint::onSocketPollStopped(int fd) {}
void FakeUdpEndpoint::onSocketReadable(int fd) {}
void FakeUdpEndpoint::onSocketWriteable(int fd) {}
void FakeUdpEndpoint::onSocketShutdown(int fd) {}

} // namespace emulator
