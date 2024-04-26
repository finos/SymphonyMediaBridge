#include "TcpEndpointImpl.h"
#include "dtls/SslDtls.h"
#include "ice/Stun.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "utils/Function.h"
#include <arpa/inet.h>
#include <cstdint>
#include <sys/socket.h>

namespace transport
{

using namespace tcp;

namespace
{
class SendJob : public jobmanager::Job
{
public:
    SendJob(TcpEndpointImpl& endpoint, memory::UniquePacket packet, const transport::SocketAddress& target)
        : _endpoint(endpoint),
          _packet(std::move(packet)),
          _target(target)
    {
    }

    void run() override { _endpoint.internalSendTo(_target, std::move(_packet)); }

private:
    TcpEndpointImpl& _endpoint;
    memory::UniquePacket _packet;
    transport::SocketAddress _target;
};

} // namespace

RtpDepacketizer::RtpDepacketizer(int socketHandle, memory::PacketPoolAllocator& allocator)
    : fd(socketHandle),
      _receivedBytes(0),
      _allocator(allocator),
      _streamPrestine(true),
      _remoteDisconnect(false)
{
}

memory::UniquePacket RtpDepacketizer::receive()
{
    if (!isGood())
    {
        return nullptr;
    }

    int flags = MSG_DONTWAIT;
    if (_receivedBytes < sizeof(_header))
    {
        auto* buffer = reinterpret_cast<uint8_t*>(&_header);
        int received = ::recv(fd, buffer + _receivedBytes, sizeof(_header) - _receivedBytes, flags);

        if (received == 0)
        {
            _remoteDisconnect = true;
            return nullptr;
        }
        if (received > 0)
        {
            _receivedBytes += received;
        }
        if (_receivedBytes < sizeof(_header))
        {
            return nullptr;
        }
        if (_header.get() >= memory::Packet::size)
        {
            // attack with malicious length specifier
            _streamPrestine = false;
            return memory::makeUniquePacket(_allocator);
        }
    }

    if (!_incompletePacket)
    {
        _incompletePacket = memory::makeUniquePacket(_allocator);
    }

    if (_incompletePacket != nullptr)
    {
        int receivedBytes = ::recv(fd,
            _incompletePacket->get() + _incompletePacket->getLength(),
            _header.get() - _incompletePacket->getLength(),
            flags);

        if (receivedBytes > 0)
        {
            _incompletePacket->setLength(_incompletePacket->getLength() + receivedBytes);
            if (_incompletePacket->getLength() == _header.get())
            {
                _receivedBytes = 0;
                return std::move(_incompletePacket);
            }
        }
        else if (receivedBytes == 0)
        {
            _remoteDisconnect = true;
            _incompletePacket.reset();
            return nullptr;
        }
    }

    return memory::UniquePacket();
}

void RtpDepacketizer::close()
{
    ::close(fd);
    fd = -1;
}

// Used for accepted socket
TcpEndpointImpl::TcpEndpointImpl(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    RtcePoll& epoll,
    int fd,
    const SocketAddress& localPort,
    const SocketAddress& peerPort)
    : _state(State::CONNECTED),
      _name("TcpEndpointImpl"),
      _socket(fd, localPort),
      _depacketizer(fd, allocator),
      _peerPort(peerPort),
      _receiveJobs(jobManager, 16),
      _sendJobs(jobManager, 512),
      _allocator(allocator),
      _defaultListener(nullptr),
      _epoll(epoll),
      _epollCountdown(2),
      _stopListener(nullptr)
{
    logger::info("accepted %s-%s", _name.c_str(), localPort.toString().c_str(), peerPort.toString().c_str());
}

// Used for connecting client socket
TcpEndpointImpl::TcpEndpointImpl(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    SocketAddress localInterface,
    RtcePoll& epoll)
    : _state(CLOSED),
      _name("TcpEndpointImpl"),
      _depacketizer(-1, allocator),
      _localInterface(localInterface),
      _receiveJobs(jobManager, 16),
      _sendJobs(jobManager, 512),
      _allocator(allocator),
      _defaultListener(nullptr),
      _epoll(epoll),
      _epollCountdown(2)
{
    int rc = _socket.open(localInterface, 0, SOCK_STREAM);
    if (rc)
    {
        logger::warn("failed to bind to %s err (%d) %s",
            _name.c_str(),
            localInterface.toString().c_str(),
            rc,
            _socket.explain(rc));
        return;
    }
    _depacketizer.fd = _socket.fd();
    logger::info("created %s", _name.c_str(), _socket.getBoundPort().toString().c_str());
    _state = State::CREATED;
}

TcpEndpointImpl::~TcpEndpointImpl()
{
    logger::debug("removed", _name.c_str());
}

void TcpEndpointImpl::connect(const SocketAddress& remotePort)
{
    if (_state == State::CREATED)
    {
        _peerPort = remotePort;
        if (_socket.isGood())
        {
            _state = State::CONNECTING;
            _epoll.add(_socket.fd(), this);
        }
    }
    else
    {
        logger::warn("connect attempt already pending. Cannot connect to %s",
            _name.c_str(),
            remotePort.toString().c_str());
    }
}

void TcpEndpointImpl::sendStunTo(const transport::SocketAddress& target,
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

    auto packet = memory::makeUniquePacket(_allocator, data, len);
    if (packet)
    {
        sendTo(target, std::move(packet));
    }
}

void TcpEndpointImpl::sendTo(const transport::SocketAddress& target, memory::UniquePacket packet)
{
    assert(!memory::PacketPoolAllocator::isCorrupt(packet.get()));
    if (_state == State::CONNECTING || _state == State::CONNECTED)
    {
        if (!_sendJobs.addJob<SendJob>(*this, std::move(packet), target))
        {
            logger::warn("failed to add SendJob", _name.c_str());
        }
    }
}

void TcpEndpointImpl::internalSendTo(const transport::SocketAddress& target, memory::UniquePacket packet)
{
    if (_state == State::CONNECTING)
    {
        if (!_pendingStunRequest)
        {
            _pendingStunRequest = std::move(packet);
        }
        else
        {
            logger::warn("discarding pending packet on tcp endpoint", _name.c_str());
        }
        return;
    }
    else if (_state != State::CONNECTED)
    {
        logger::debug("discarding packet. Socket not open", _name.c_str());
        return;
    }

    if (_pendingStunRequest)
    {
        continueSend();
    }

    sendPacket(*packet);
}

void TcpEndpointImpl::continueSend()
{
    _socket.updateBoundPort();
    if (_pendingStunRequest && _state == State::CONNECTED)
    {
        // stun requests are always created on own allocator in SendStunRequest
        sendPacket(*_pendingStunRequest);

        _pendingStunRequest.reset();
    }
}

void TcpEndpointImpl::sendPacket(const memory::Packet& packet)
{
    if (_remainder.getLength() > 0)
    {
        size_t bytesSent;
        auto rc = _socket.sendAggregate(_remainder.get(), _remainder.getLength(), bytesSent);
        if (bytesSent < _remainder.getLength())
        {
            std::memmove(_remainder.get(),
                reinterpret_cast<uint8_t*>(_remainder.get()) + bytesSent,
                _remainder.getLength() - bytesSent);
            _remainder.setLength(_remainder.getLength() - bytesSent);
            logger::warn("discarding packet, err %d %s", _name.c_str(), rc, _socket.explain(rc));
            return;
        }
        else
        {
            _remainder.setLength(0);
        }
    }

    size_t bytesSent = 0;
    nwuint16_t shim(packet.getLength());
    auto rc = _socket.sendAggregate(&shim, sizeof(uint16_t), packet.get(), packet.getLength(), bytesSent);
    if (bytesSent <= 1)
    {
        auto data = reinterpret_cast<uint8_t*>(&shim);
        _remainder.append(data + bytesSent, sizeof(shim) - bytesSent);
        _remainder.append(packet.get(), packet.getLength());
        logger::debug("partial packet sent %zu / %zu, err %d %s",
            _name.c_str(),
            bytesSent,
            packet.getLength(),
            rc,
            _socket.explain(rc));
    }
    else if (bytesSent < packet.getLength() + sizeof(shim))
    {
        bytesSent -= sizeof(uint16_t);
        _remainder.append(packet.get() + bytesSent, packet.getLength() - bytesSent);
        logger::debug("partial packet sent %zu / %zu, err %d %s",
            _name.c_str(),
            bytesSent,
            packet.getLength(),
            rc,
            _socket.explain(rc));
    }
}

// starts a sequence to
// - unregister from rtcepoll incoming data
// - await pending receive jobs to complete
// - await pending send jobs to complete
void TcpEndpointImpl::stop(Endpoint::IStopEvents* listener)
{
    bool hasStoppingOwnership = false;
    auto state = _state.load();

    while (state == State::CONNECTING || state == State::CONNECTED)
    {
        if (_state.compare_exchange_weak(state, State::STOPPING))
        {
            _stopListener = listener;
            _epoll.remove(_socket.fd(), this);
            hasStoppingOwnership = true;
            break;
        }
    }

    // When we don't have the stopping ownership
    // we need to sync with _receiveJobs to call `onEndpointStopped`
    // otherwise we can have a race condition when the socket is terminated
    // by the remote side at the same time
    if (!hasStoppingOwnership)
    {
        _receiveJobs.post([listener, this]() {
            if (_state.load() == State::CREATED)
            {
                listener->onEndpointStopped(this);
            }
            else
            {
                _stopListener = listener;
            }
        });
    }
}

// closed from remote side
// read pending data
// then start close port procedure
void TcpEndpointImpl::onSocketShutdown(int fd)
{
    if (_depacketizer.fd == fd)
    {
        auto state = _state.load();
        // Avoid race conditions whith endpoint stop(Endpoint::IStopEvents* listener)
        // called when the TcpSocket is being deleted by SMB at the same time we receive
        // a close request from remote side
        while (state == State::CONNECTING || state == State::CONNECTED)
        {
            if (_state.compare_exchange_weak(state, State::STOPPING))
            {
                logger::debug("peer shut down socket STOPPING", _name.c_str());
                if (!_receiveJobs.post(utils::bind(&TcpEndpointImpl::internalReceive, this, _depacketizer.fd)))
                {
                    logger::warn("failed to add ReceiveJob", _name.c_str());
                }

                _epoll.remove(fd, this);
            }
        }
    }
}

void TcpEndpointImpl::onSocketPollStarted(int fd)
{
    if (_state == State::CONNECTING && !_peerPort.empty())
    {
        int rc = _socket.connect(_peerPort);
        if (rc != 0)
        {
            logger::error("failed to connect to %s (%d) %s",
                _name.c_str(),
                _peerPort.toString().c_str(),
                rc,
                _socket.explain(rc));
        }
    }
}

void TcpEndpointImpl::onSocketPollStopped(int fd)
{
    assert(_epollCountdown.load() == 2);
    _epollCountdown = 2; // We set it anyway
    _sendJobs.addJob<tcp::PortStoppedJob<TcpEndpointImpl, JobContext::SEND_JOBS>>(*this, _epollCountdown);
    _receiveJobs.addJob<tcp::PortStoppedJob<TcpEndpointImpl, JobContext::RECEIVE_JOBS>>(*this, _epollCountdown);
}

void TcpEndpointImpl::internalStopped(JobContext jobContext)
{
    _state = State::CREATED;

    auto callEndpointStopFn = [this]() {
        if (_stopListener)
        {
            _stopListener->onEndpointStopped(this);
        }
    };

    // the _stopListener sync is made over receiveJobs queue
    // and this method can be called either on receiveJobs queue context
    // or sendJobs queue context.
    // When it is called on the sendJobs, we need to post a job on receiveJob to avoid
    // race conditions with set the _stopListener pointer on onStop
    if (jobContext == JobContext::RECEIVE_JOBS)
    {
        callEndpointStopFn();
    }
    else
    {
        _receiveJobs.post(callEndpointStopFn);
    }
}

void TcpEndpointImpl::onSocketReadable(int fd)
{
    if (fd == _depacketizer.fd)
    {
        if (!_pendingRead.test_and_set())
        {
            if (!_receiveJobs.post(utils::bind(&TcpEndpointImpl::internalReceive, this, _depacketizer.fd)))
            {
                logger::warn("failed to add Receivejob", _name.c_str());
            }
        }
    }
}

void TcpEndpointImpl::onSocketWriteable(int fd)
{
    if (fd == _depacketizer.fd && _state == State::CONNECTING)
    {
        _state = State::CONNECTED;
        logger::debug("connected to %s", _name.c_str(), _peerPort.toString().c_str());
        if (!_sendJobs.post(utils::bind(&TcpEndpointImpl::continueSend, this)))
        {
            logger::warn("failed to add ContinueSendJob", _name.c_str());
        }
    }
}

void TcpEndpointImpl::unregisterListener(IEvents* listener)
{
    if (listener == _defaultListener)
    {
        _defaultListener = nullptr;
        _receiveJobs.post([this, listener]() { listener->onUnregistered(std::ref(*this)); });
    }
}

void TcpEndpointImpl::internalReceive(int fd)
{
    _pendingRead.clear();
    while (true)
    {
        TcpEndpointImpl::IEvents* listener = _defaultListener;
        auto packet = _depacketizer.receive();
        if (!packet)
        {
            if (listener && _depacketizer.hasRemoteDisconnected())
            {
                listener->onTcpDisconnect(*this);
            }
            break;
        }

        if (!listener)
        {
            continue;
        }
        const uint64_t receiveTime = utils::Time::getAbsoluteTime();
        if (ice::isStunMessage(packet->get(), packet->getLength()))
        {
            listener->onIceReceived(*this, _peerPort, _socket.getBoundPort(), std::move(packet), receiveTime);
            continue;
        }
        else if (transport::isDtlsPacket(packet->get(), packet->getLength()))
        {
            listener->onDtlsReceived(*this, _peerPort, _socket.getBoundPort(), std::move(packet), receiveTime);
            continue;
        }
        else if (rtp::isRtcpPacket(packet->get(), packet->getLength()))
        {
            auto rtcpReport = rtp::RtcpReport::fromPtr(packet->get(), packet->getLength());
            if (rtcpReport)
            {
                listener->onRtcpReceived(*this, _peerPort, _socket.getBoundPort(), std::move(packet), receiveTime);
                continue;
            }
        }
        else if (rtp::isRtpPacket(packet->get(), packet->getLength()))
        {
            auto rtpPacket = rtp::RtpHeader::fromPacket(*packet);
            if (rtpPacket)
            {
                listener->onRtpReceived(*this, _peerPort, _socket.getBoundPort(), std::move(packet), receiveTime);
                continue;
            }
        }
        else
        {
            logger::warn("unexpected packet %zu", _name.c_str(), packet->getLength());
            onSocketShutdown(fd);
        }
    }
}

// used when routing is not possible and there is a single owner of the endpoint
void TcpEndpointImpl::registerDefaultListener(IEvents* listener)
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

void TcpEndpointImpl::registerListener(const std::string& stunUserName, IEvents* listener)
{
    registerDefaultListener(listener);
}

// registration of DTLS listener is automatic when ICE is used
void TcpEndpointImpl::registerListener(const SocketAddress& srcAddress, IEvents* listener)
{
    registerDefaultListener(listener);
}

// already added to epoll in constructor or on accept
void TcpEndpointImpl::start() {}

bool TcpEndpointImpl::configureBufferSizes(size_t sendBufferSize, size_t receiveBufferSize)
{
    logger::debug("tcp endpoint buffer sizes send %zu, recv %zu", _name.c_str(), sendBufferSize, receiveBufferSize);
    return 0 == _socket.setSendBuffer(sendBufferSize) && 0 == _socket.setReceiveBuffer(receiveBufferSize);
}

SocketAddress TcpEndpointImpl::getLocalPort() const
{
    const State currentState = _state.load();
    if (currentState == State::CONNECTED)
    {
        return _socket.getBoundPort();
    }
    else if (currentState != State::CLOSED)
    {
        return _localInterface;
    }

    return transport::SocketAddress();
}

} // namespace transport
