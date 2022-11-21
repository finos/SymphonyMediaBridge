#include "FakeUdpEndpoint.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "transport/EndpointFactoryImpl.h"
#include "transport/UdpEndpoint.h"
#include "transport/dtls/SslDtls.h"

namespace
{
using namespace emulator;
class UnRegisterListenerJob : public jobmanager::Job
{
public:
    UnRegisterListenerJob(FakeUdpEndpoint& endpoint, transport::Endpoint::IEvents* listener)
        : _endpoint(endpoint),
          _listener(listener)
    {
    }

    void run() override { _endpoint.internalUnregisterListener(_listener); }

private:
    FakeUdpEndpoint& _endpoint;
    transport::Endpoint::IEvents* _listener;
};

class UnRegisterNotifyListenerJob : public jobmanager::Job
{
public:
    UnRegisterNotifyListenerJob(FakeUdpEndpoint& endpoint, transport::Endpoint::IEvents& listener)
        : _endpoint(endpoint),
          _listener(listener)
    {
    }

    void run() override { _listener.onUnregistered(_endpoint); }

private:
    FakeUdpEndpoint& _endpoint;
    transport::Endpoint::IEvents& _listener;
};

class ReceiveJob : public jobmanager::Job
{
public:
    ReceiveJob(FakeUdpEndpoint& endpoint) : _endpoint(endpoint) {}

    void run() override { _endpoint.internalReceive(); }

private:
    FakeUdpEndpoint& _endpoint;
};

template <typename KeyType>
transport::UdpEndpointImpl::IEvents* findListener(
    concurrency::MpmcHashmap32<KeyType, transport::UdpEndpointImpl::IEvents*>& map,
    const KeyType& key)
{
    auto it = map.find(key);
    if (it != map.cend())
    {
        return it->second;
    }
    return nullptr;
}

} // namespace

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
      _receiveJobs(jobManager, 16),
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
    if (msg->header.isRequest() && !_iceResponseListeners.contains(transactionId) && !_dtlsListeners.contains(target))
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
    // Hashmap allows erasing elements while iterating.
    auto itPair = _iceResponseListeners.find(transactionId);
    if (itPair != _iceResponseListeners.cend())
    {
        _iceResponseListeners.erase(transactionId);
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

    _iceListeners.emplace(stunUserName, listener);
    listener->onRegistered(*this);
}

void FakeUdpEndpoint::registerListener(const transport::SocketAddress& srcAddress, IEvents* listener)
{
    auto dtlsIt = _dtlsListeners.find(srcAddress);
    if (dtlsIt != _dtlsListeners.end())
    {
        // src port is re-used. Unregister will look at listener pointer
        if (dtlsIt->second == listener)
        {
            return;
        }

        if (dtlsIt->second)
        {
            _receiveJobs.addJob<UnRegisterNotifyListenerJob>(*this, *dtlsIt->second);
        }
        dtlsIt->second = listener;
        listener->onRegistered(*this);
    }
    else
    {
        _dtlsListeners.emplace(srcAddress, listener);
        listener->onRegistered(*this);
    }
}

void FakeUdpEndpoint::registerDefaultListener(IEvents* defaultListener)
{
    _defaultListener = defaultListener;
}

void FakeUdpEndpoint::unregisterListener(IEvents* listener)
{
    if (!_receiveJobs.addJob<UnRegisterListenerJob>(*this, listener))
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
            _receiveJobs.addJob<UnRegisterNotifyListenerJob>(*this, *listener);
        }
    }

    for (auto& responseListener : _iceResponseListeners)
    {
        if (responseListener.second == listener)
        {
            _iceResponseListeners.erase(responseListener.first);
            // must 7be iceListener to be iceResponseListener so no extra unreg notification
        }
    }

    for (auto& item : _dtlsListeners)
    {
        if (item.second == listener)
        {
            _dtlsListeners.erase(item.first);
            _receiveJobs.addJob<UnRegisterNotifyListenerJob>(*this, *listener);
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
            if (!_receiveJobs.addJob<ReceiveJob>(*this))
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
    transport::UdpEndpointImpl::IEvents* listener = _defaultListener;

    if (ice::isStunMessage(packet->get(), packet->getLength()))
    {
        auto msg = ice::StunMessage::fromPtr(packet->get());

        if (msg->header.isRequest())
        {
            auto users = msg->getAttribute<ice::StunUserName>(ice::StunAttribute::USERNAME);
            if (users)
            {
                auto userName = users->getNames().first;
                listener = findListener(_iceListeners, userName);
            }
            logger::debug("ICE request to %s src %s",
                _name.c_str(),
                users->getNames().first.c_str(),
                srcAddress.toString().c_str());
        }
        else if (msg->header.isResponse())
        {
            auto transactionId = msg->header.transactionId.get();
            listener = findListener(_iceResponseListeners, transactionId);
            if (listener)
            {
                _iceResponseListeners.erase(transactionId);
            }
            else
            {
                listener = findListener(_dtlsListeners, srcAddress);
            }
        }
        if (listener)
        {
            listener->onIceReceived(*this, srcAddress, _localPort, std::move(packet), timestamp);
            return;
        }
    }
    else if (transport::isDtlsPacket(packet->get()))
    {
        listener = findListener(_dtlsListeners, srcAddress);
        listener = listener ? listener : _defaultListener.load();
        if (listener)
        {
            listener->onDtlsReceived(*this, srcAddress, _localPort, std::move(packet), timestamp);
            return;
        }
    }
    else if (rtp::isRtcpPacket(packet->get(), packet->getLength()))
    {
        auto rtcpReport = rtp::RtcpReport::fromPtr(packet->get(), packet->getLength());
        if (rtcpReport)
        {
            listener = findListener(_dtlsListeners, srcAddress);

            if (listener)
            {
                listener->onRtcpReceived(*this, srcAddress, _localPort, std::move(packet), timestamp);
                return;
            }
        }
    }
    else if (rtp::isRtpPacket(packet->get(), packet->getLength()))
    {
        auto rtpPacket = rtp::RtpHeader::fromPacket(*packet);
        if (rtpPacket)
        {
            listener = findListener(_dtlsListeners, srcAddress);

            if (listener)
            {
                listener->onRtpReceived(*this, srcAddress, _localPort, std::move(packet), timestamp);
                return;
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
