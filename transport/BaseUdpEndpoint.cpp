#include "transport/BaseUdpEndpoint.h"

namespace transport
{
namespace
{
using namespace transport;
class SendJob : public jobmanager::Job
{
public:
    explicit SendJob(BaseUdpEndpoint& endpoint) : _endpoint(endpoint) {}

    void run() override { _endpoint.internalSend(); }

private:
    BaseUdpEndpoint& _endpoint;
};
class ReceiveJob : public jobmanager::Job
{
public:
    ReceiveJob(BaseUdpEndpoint& endpoint, int fd) : _endpoint(endpoint), _fd(fd) {}

    void run() override
    {
#ifdef __APPLE__
        _endpoint.internalReceive(_fd, 1);
#else
        _endpoint.internalReceive(_fd, 400);
#endif
    }

private:
    BaseUdpEndpoint& _endpoint;
    int _fd;
};

class StopPortJob : public jobmanager::Job
{
public:
    StopPortJob(BaseUdpEndpoint& endpoint, std::atomic_uint32_t& countDown) : _endpoint(endpoint), _countDown(countDown)
    {
    }

    void run() override
    {
        auto value = --_countDown;
        if (value == 0)
        {
            _endpoint.internalStopped();
        }
    }

private:
    BaseUdpEndpoint& _endpoint;
    std::atomic_uint32_t& _countDown;
};
} // namespace

BaseUdpEndpoint::BaseUdpEndpoint(const char* name,
    jobmanager::JobManager& jobManager,
    size_t maxSessionCount,
    memory::PacketPoolAllocator& allocator,
    const SocketAddress& localPort,
    RtcePoll& epoll,
    bool isShared)
    : _state(Endpoint::CLOSED),
      _name(name),
      _localPort(localPort),
      _receiveJobs(jobManager, 16),
      _sendJobs(jobManager, 16),
      _allocator(allocator),
      _sendQueue(maxSessionCount * 256),
      _epoll(epoll),
      _epollCountdown(2),
      _isShared(isShared),
      _defaultListener(nullptr),
      _receiveTracker(utils::Time::ms * 100),
      _sendTracker(utils::Time::ms * 100)
{
    _pendingRead.clear();
    _pendingSend.clear();
    _isFull.clear();
    auto result = _socket.open(localPort, localPort.getPort());
    if (result == 0)
    {
        _state = Endpoint::CREATED;
    }
}

BaseUdpEndpoint::~BaseUdpEndpoint()
{
    if (_socket.isGood())
    {
        _socket.close();
    }
}

void BaseUdpEndpoint::internalStopped()
{
    assert(_epollCountdown == 0);
    _state = State::CREATED;
    const auto defaultListener = _defaultListener.load();
    if (defaultListener)
    {
        defaultListener->onEndpointStopped(*this);
    }
}

void BaseUdpEndpoint::sendTo(const transport::SocketAddress& target, memory::UniquePacket packet)
{
    if (!packet)
    {
        return;
    }

    if (target.getFamily() != _localPort.getFamily())
    {
        logger::debug("incompatible target address", _name.c_str());
        return;
    }

    assert(!memory::PacketPoolAllocator::isCorrupt(packet.get()));
    if (_sendQueue.push({target, std::move(packet)}))
    {
        if (!_pendingSend.test_and_set())
        {
            _sendJobs.addJob<SendJob>(*this);
        }
    }
}

void BaseUdpEndpoint::internalSend()
{
    _pendingSend.clear(); // intend to send all
    const size_t batchSize = 400;
    OutboundPacket packetInfo[batchSize];
    RtcSocket::Message messages[batchSize];
    const auto start = utils::Time::getAbsoluteTime();
    uint32_t packetCounter = 0;
    for (; _state == Endpoint::CONNECTED;)
    {
        size_t count = 0;
        size_t byteCount = 0;
        for (; count < batchSize && _sendQueue.pop(packetInfo[count]); ++count)
        {
            messages[count].fragmentCount = 0;
            auto& packet = packetInfo[count].packet;
            messages[count].target = &packetInfo[count].target;
            messages[count].add(packet->get(), packet->getLength());
            byteCount += packet->getLength();
        }
        packetCounter += count;
        if (count == 0)
        {
            _isFull.clear();
            break;
        }

        const auto sendTimestamp = utils::Time::getAbsoluteTime();
        auto errorCount = _socket.sendMultiple(messages, count);
        for (size_t i = 0; errorCount > 0 && i < count; ++i)
        {
            const auto rc = messages[i].errorCode;
            if (rc == EMSGSIZE)
            {
                const auto packetSize = messages[i].getLength();
                if (packetSize >= 1480)
                {
                    logger::warn("err (%d) failed sending to %s, size %zu",
                        _name.c_str(),
                        rc,
                        messages[i].target->toString().c_str(),
                        packetSize);
                }

                byteCount -= packetSize;
            }
            else if (messages[i].errorCode != 0)
            {
                logger::warn("err (%d) failed sending to %s, %s",
                    _name.c_str(),
                    rc,
                    messages[i].target->toString().c_str(),
                    transport::RtcSocket::explain(rc));

                byteCount -= messages[i].getLength();
            }
        }

        _sendTracker.update(byteCount, sendTimestamp);
    }
    if (packetCounter > 10200)
    {
        const auto duration = utils::Time::diff(start, utils::Time::getAbsoluteTime());
        logger::info("sent %u packets in loop %" PRIu64 "pps",
            _name.c_str(),
            packetCounter,
            packetCounter * utils::Time::sec / duration);
    }
}

bool BaseUdpEndpoint::openPort(uint16_t port)
{
    _socket.close();
    _localPort.setPort(port);
    auto result = _socket.open(_localPort, port, SOCK_DGRAM);
    if (result == 0)
    {
        _state = Endpoint::State::CREATED;
    }
    return result == 0;
}

// starts a sequence to
// - unregister from rtcepoll incoming data
// - await pending receive jobs to complete
// - await pending send jobs to complete
// - close socket
// - report on IEvents that port has closed
void BaseUdpEndpoint::stop()
{
    if (_state == Endpoint::State::CONNECTING || _state == Endpoint::State::CONNECTED)
    {
        _state = Endpoint::State::STOPPING;
        _epollCountdown = 2;
        if (!_epoll.remove(_socket.fd(), this))
        {
            logger::error("Failed to request epoll unregistration", _name.c_str());
        }
    }
}

void BaseUdpEndpoint::onSocketPollStarted(int fd)
{
    if (_state == Endpoint::State::CONNECTING)
    {
        _state = Endpoint::State::CONNECTED;
    }
}

void BaseUdpEndpoint::onSocketPollStopped(int fd)
{
    if (!_receiveJobs.addJob<StopPortJob>(*this, _epollCountdown))
    {
        logger::error("failed to add poll stop job", _name.c_str());
    }
    if (!_sendJobs.addJob<StopPortJob>(*this, _epollCountdown))
    {
        logger::error("failed to add poll stop job", _name.c_str());
    }
}

void BaseUdpEndpoint::onSocketReadable(int fd)
{
    if (!_pendingRead.test_and_set())
    {
        if (!_receiveJobs.addJob<ReceiveJob>(*this, fd))
        {
            logger::warn("receive queue full", _name.c_str());
        }
    }
}

EndpointMetrics BaseUdpEndpoint::getMetrics(uint64_t timestamp) const
{
    return EndpointMetrics(_sendQueue.size(),
        _receiveTracker.get(timestamp, utils::Time::sec) * 8 * utils::Time::ms,
        _sendTracker.get(timestamp, utils::Time::sec) * 8 * utils::Time::ms);
}

namespace
{
#ifdef __APPLE__
struct mmsghdr
{
    struct msghdr msg_hdr;
    unsigned int msg_len; // number of received bytes for header
};
#endif

struct ReceivedMessage
{
    transport::RawSockAddress src_addr;
    iovec iobuffer;
    memory::UniquePacket packet;

    bool link(mmsghdr& header, memory::UniquePacket packetPtr)
    {
        if (!packetPtr)
        {
            return false;
        }
        packet.swap(packetPtr);
        iobuffer.iov_base = packet->get();
        iobuffer.iov_len = memory::Packet::size;

        header.msg_hdr.msg_control = nullptr;
        header.msg_hdr.msg_controllen = 0;
        header.msg_hdr.msg_flags = MSG_DONTWAIT;
        header.msg_hdr.msg_iov = &iobuffer;
        header.msg_hdr.msg_iovlen = 1;
        header.msg_hdr.msg_name = &src_addr;
        header.msg_hdr.msg_namelen = sizeof(src_addr);

        header.msg_len = 0;

        return true;
    }
};
} // namespace

void BaseUdpEndpoint::internalReceive(const int fd, const uint32_t batchSize)
{
    ReceivedMessage receiveMessage[batchSize];
    mmsghdr messageHeader[batchSize];

    const int flags = MSG_DONTWAIT;

    _pendingRead.clear(); // one extra job may be added after us
    uint32_t packetCount = 0;
    uint32_t limit = 1;
    while (true)
    {
        for (uint32_t i = packetCount; i < limit; ++i)
        {
            if (!receiveMessage[i].link(messageHeader[i], memory::makeUniquePacket(_allocator)))
            {
                break;
            }
            ++packetCount;
        }
        if (packetCount == 0)
        {
            logger::warn("cannot receive, packet allocator depleted", _socket.getBoundPort().toString().c_str());
            break;
        }

        if (packetCount == 1)
        {
            ssize_t byteCount = ::recvmsg(fd, &messageHeader[0].msg_hdr, flags);
            _receiveTracker.update(byteCount, utils::Time::getAbsoluteTime());
            if (byteCount <= 0)
            {
                break;
            }
            else if (byteCount >= static_cast<ssize_t>(memory::Packet::size))
            {
                byteCount = 0; // Attack with Jumbo frame. Discard
            }

            receiveMessage[0].packet->setLength(byteCount);
            dispatchReceivedPacket(SocketAddress(&receiveMessage[0].src_addr.gen, nullptr),
                std::move(receiveMessage[0].packet));
            packetCount = 0;
#ifndef __APPLE__
            limit = std::min(batchSize, 2u);
#endif
        }
        else
        {
#ifdef __APPLE__
            int count = 0;
#else
            const auto count = ::recvmmsg(fd, messageHeader, packetCount, flags, nullptr);
#endif
            if (count <= 0)
            {
                break;
            }
            const auto receiveTime = utils::Time::getAbsoluteTime();
            for (int i = 0; i < count; ++i)
            {
                _receiveTracker.update(messageHeader[i].msg_len, receiveTime);
                if (messageHeader[i].msg_len < memory::Packet::size)
                {
                    receiveMessage[i].packet->setLength(messageHeader[i].msg_len);
                }
                else
                {
                    receiveMessage[i].packet->setLength(0); // Attack with Jumbo frame. Discard.
                }
                dispatchReceivedPacket(SocketAddress(&receiveMessage[i].src_addr.gen, nullptr),
                    std::move(receiveMessage[i].packet));
            }
            for (uint32_t i = 0; i < packetCount - count; ++i)
            {
                receiveMessage[i].link(messageHeader[i], std::move(receiveMessage[i + count].packet));
            }
            if (count == static_cast<int>(packetCount))
            {
                limit = std::min(batchSize, packetCount * 2);
            }
            else
            {
                limit = std::min(batchSize, 2u);
            }

            packetCount -= count;
        }
    }
}

// used when routing is not possible and there is a single owner of the endpoint
void BaseUdpEndpoint::registerDefaultListener(IEvents* defaultListener)
{
    _defaultListener = defaultListener;
}

// enables packet reception
void BaseUdpEndpoint::start()
{
    if (_state == Endpoint::State::CREATED)
    {
        _state = Endpoint::State::CONNECTING;
        _epoll.add(_socket.fd(), this);
    }
}

bool BaseUdpEndpoint::configureBufferSizes(size_t sendBufferSize, size_t receiveBufferSize)
{
    return (0 == _socket.setSendBuffer(sendBufferSize)) && (0 == _socket.setReceiveBuffer(receiveBufferSize));
}

} // namespace transport
