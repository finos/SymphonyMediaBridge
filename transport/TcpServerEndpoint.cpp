#include "TcpServerEndpoint.h"
#include "dtls/SslDtls.h"
#include "ice/Stun.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "utils/Time.h"
#include <arpa/inet.h>
#include <cstdint>
#include <sys/socket.h>
namespace transport
{

namespace
{
class UnRegisterServerListenerJob : public jobmanager::Job
{
public:
    UnRegisterServerListenerJob(TcpServerEndpoint& endpoint,
        const std::string& userName,
        ServerEndpoint::IEvents* listener)
        : _userName(userName),
          _endpoint(endpoint),
          _listener(listener)
    {
    }

    void run() override { _endpoint.internalUnregisterListener(_userName, _listener); }

private:
    const std::string _userName;
    TcpServerEndpoint& _endpoint;
    ServerEndpoint::IEvents* _listener;
};

class AcceptJob : public jobmanager::Job
{
public:
    AcceptJob(TcpServerEndpoint& endpoint) : _endpoint(endpoint) {}

    void run() override { _endpoint.internalAccept(); }

private:
    TcpServerEndpoint& _endpoint;
};
} // namespace

class EarlyShutdownJob : public jobmanager::Job
{
public:
    EarlyShutdownJob(TcpServerEndpoint& endpoint, int fd) : _endpoint(endpoint), _fd(fd) {}

    void run() override { _endpoint.internalShutdown(_fd); }

private:
    TcpServerEndpoint& _endpoint;
    int _fd;
};

class ClosePendingSocketJob : public jobmanager::Job
{
public:
    ClosePendingSocketJob(TcpServerEndpoint& endPoint, int fd) : _fd(fd), _endPoint(endPoint) {}

    void run() override { _endPoint.internalClosePendingSocket(_fd); }

private:
    int _fd;
    TcpServerEndpoint& _endPoint;
};

TcpServerEndpoint::PendingTcp::PendingTcp(int fd,
    memory::PacketPoolAllocator& allocator,
    const SocketAddress& localPort_,
    const SocketAddress& peerPort_)
    : packetizer(fd, allocator),
      localPort(localPort_),
      peerPort(peerPort_),
      acceptTime(utils::Time::getAbsoluteTime())
{
}

TcpServerEndpoint::TcpServerEndpoint(jobmanager::JobManager& jobManager,
    memory::PacketPoolAllocator& allocator,
    RtcePoll& rtcePoll,
    size_t maxSessions,
    IEvents* listener,
    const SocketAddress& localPort,
    const config::Config& config)
    : _state(Endpoint::State::CLOSED),
      _name("TcpServerEndpoint"),
      _receiveJobs(jobManager),
      _allocator(allocator),
      _pendingConnections(maxSessions / 2),
      _pendingEpollRegistrations(0),
      _blackList(512),
      _pendingConnectCounters(maxSessions / 4),
      _iceListeners(maxSessions),
      _epoll(rtcePoll),
      _listener(listener),
      _config(config)
{
    int rc = _socket.open(localPort, localPort.getPort(), SOCK_STREAM);
    if (rc)
    {
        logger::error("open server socket %s failed (%d) %s",
            "TcpServerEndpoint",
            localPort.toString().c_str(),
            rc,
            _socket.explain(rc));
        return;
    }
    _state = Endpoint::State::CREATED;
    _socket.setSendBuffer(128 * 1024);
    _socket.setReceiveBuffer(128 * 1024);
    logger::info("server port %s", _name.c_str(), _socket.getBoundPort().toString().c_str());
}

TcpServerEndpoint::~TcpServerEndpoint()
{
    assert(!_socket.isGood()); // must close first
    logger::info("removed", _name.c_str());
}

void TcpServerEndpoint::start()
{
    if (_state == Endpoint::State::CREATED)
    {
        _state = Endpoint::State::CONNECTING;
        _epoll.add(_socket.fd(), this);
        int rc = _socket.listen(16);
        if (rc != 0)
        {
            close();
        }
    }
}

void TcpServerEndpoint::close()
{
    if (_state != Endpoint::State::CLOSING && _state != Endpoint::State::CLOSED)
    {
        _state = Endpoint::State::CLOSING;
        _epoll.remove(_socket.fd(), this);
    }
}

void TcpServerEndpoint::registerListener(const std::string& stunUserName, Endpoint::IEvents* listener)
{
    _iceListeners.emplace(stunUserName, listener);
}

void TcpServerEndpoint::unregisterListener(const std::string& stunUserName, ServerEndpoint::IEvents* listener)
{
    _receiveJobs.addJob<UnRegisterServerListenerJob>(*this, stunUserName, listener);
}

void TcpServerEndpoint::internalUnregisterListener(const std::string& stunUserName, ServerEndpoint::IEvents* listener)
{
    _iceListeners.erase(stunUserName);
    listener->onServerPortUnregistered(*this);
}

void TcpServerEndpoint::onSocketPollStarted(int fd)
{
    if (fd == _socket.fd())
    {
        if (_state == Endpoint::State::CONNECTING)
        {
            _state = Endpoint::State::CONNECTED;
        }
        logger::info("tcp listening on %s", _name.c_str(), _socket.getBoundPort().toString().c_str());
    }
}

void TcpServerEndpoint::onSocketPollStopped(int fd)
{
    if (fd == _socket.fd())
    {
        logger::info("server events stopped on %s", _name.c_str(), _socket.getBoundPort().toString().c_str());
        _receiveJobs.addJob<tcp::ClosePortJob<TcpServerEndpoint>>(*this, 1);
    }
    else
    {
        _receiveJobs.addJob<ClosePendingSocketJob>(*this, fd);
    }
}

void TcpServerEndpoint::onSocketReadable(int fd)
{
    if (fd == _socket.fd())
    {
        _receiveJobs.addJob<AcceptJob>(*this);
    }
    else
    {
        _receiveJobs.addJob<tcp::ReceiveJob<TcpServerEndpoint>>(*this, fd);
    }
}

// erase old connection attempts
// If at 90% capacity, identify most frequent peer ip and black list it
void TcpServerEndpoint::cleanupStaleConnections()
{
    auto timestamp = utils::Time::getAbsoluteTime();

    for (auto item : _blackList)
    {
        if (utils::Time::diffGT(item.second, timestamp, utils::Time::minute * 5))
        {
            _blackList.erase(item.first);
        }
    }

    if (_pendingConnections.size() >= _pendingConnections.capacity() * 9 / 10)
    {
        _pendingConnectCounters.clear();
        uint32_t maxCount = 0;
        transport::SocketAddress maxKey;
        for (auto& itPair : _pendingConnections)
        {
            auto added = _pendingConnectCounters.emplace(transport::SocketAddress(itPair.second.peerPort, 0), 0);
            if (added.first == _pendingConnectCounters.cend())
            {
                continue;
            }
            auto counterIt = added.first;
            ++counterIt->second;
            if (counterIt->second > maxCount)
            {
                maxCount = counterIt->second;
                maxKey = counterIt->first;
            }
        }
        if (maxCount > _pendingConnections.size() / 2)
        {
            logger::warn("black listing IP %s due to massive tcp connection attempts %u",
                _name.c_str(),
                maxKey.ipToString().c_str(),
                maxCount);
            _blackList.emplace(transport::SocketAddress(maxKey, 0), timestamp);
        }

        logger::info("closing pending excessive tcp connections from %s, count %u",
            _name.c_str(),
            maxKey.ipToString().c_str(),
            maxCount);
        for (auto& itPair : _pendingConnections)
        {
            if (itPair.second.peerPort.equalsIp(maxKey))
            {
                _epoll.remove(itPair.second.packetizer.fd, this);
                _pendingConnections.erase(itPair.first);
            }
        }
    }

    for (auto& itPair : _pendingConnections)
    {
        if (utils::Time::diffGT(itPair.second.acceptTime, timestamp, _config.ice.tcp.iceTimeoutSec * utils::Time::sec))
        {
            logger::debug("closing stale tcp connection %s-%s",
                _name.c_str(),
                itPair.second.localPort.toString().c_str(),
                itPair.second.peerPort.toString().c_str());
            _epoll.remove(itPair.second.packetizer.fd, this);
            _pendingConnections.erase(itPair.first);
        }
    }
}

void TcpServerEndpoint::internalAccept()
{
    cleanupStaleConnections();
    SocketAddress peerPort;
    SocketAddress localPort;
    int clientSocket = -1;
    for (int rc = RtcSocket::accept(_socket.fd(), peerPort, localPort, clientSocket); rc != EAGAIN && rc != EWOULDBLOCK;
         rc = RtcSocket::accept(_socket.fd(), peerPort, localPort, clientSocket))
    {
        if (rc != 0)
        {
            logger::warn("failed to accept socket (%d) %s", _name.c_str(), rc, RtcSocket::explain(rc));
            continue;
        }
        if (_blackList.contains(transport::SocketAddress(peerPort, 0)))
        {
            ::close(clientSocket);
            continue;
        }

        logger::info("tcp connection accepted from %s", _name.c_str(), peerPort.toString().c_str());
        assert(!_pendingConnections.contains(clientSocket));
        auto it = _pendingConnections.emplace(clientSocket, clientSocket, _allocator, localPort, peerPort);
        if (it.second)
        {
            ++_pendingEpollRegistrations;
            _epoll.add(clientSocket, this);
        }
        else
        {
            logger::error("pending connections depleted. Tcp candidate will fail.", _name.c_str());
            ::close(clientSocket);
        }
    }
}

void TcpServerEndpoint::internalReceive(int fd)
{
    auto it = _pendingConnections.find(fd);
    if (it == _pendingConnections.end())
    {
        return; // should be spurious notify from old socket
    }

    auto& pendingTcp = it->second;
    auto* packet = pendingTcp.packetizer.receive();
    if (packet)
    {
        if (ice::isStunMessage(packet->get(), packet->getLength()))
        {
            auto msg = ice::StunMessage::fromPtr(packet->get());

            if (msg->header.isRequest())
            {
                auto users = msg->getAttribute<ice::StunUserName>(ice::StunAttribute::USERNAME);
                if (users)
                {
                    const auto names = users->getNames();
                    auto listenIt = _iceListeners.find(names.first);
                    if (listenIt != _iceListeners.end())
                    {
                        auto* endpoint = new TcpEndpoint(_receiveJobs.getJobManager(),
                            _allocator,
                            _epoll,
                            fd,
                            pendingTcp.localPort,
                            pendingTcp.peerPort);
                        _pendingConnections.erase(fd);

                        // if read event is fired now we may miss it and it is edge triggered.
                        // everything relies on that the ice session will want to respond to the request
                        _epoll.add(fd, endpoint);
                        --_pendingEpollRegistrations; // it is not ours anymore

                        listenIt->second->onIceReceived(*endpoint,
                            pendingTcp.peerPort,
                            endpoint->getLocalPort(),
                            packet,
                            _allocator);

                        _iceListeners.erase(listenIt->first);
                        return;
                    }

                    logger::debug("Unknown user %s:%s, closing tcp connection %s",
                        _name.c_str(),
                        names.first.c_str(),
                        names.second.c_str(),
                        pendingTcp.peerPort.toString().c_str());
                }
                else
                {
                    logger::debug("No user name, closing tcp connection %s",
                        _name.c_str(),
                        pendingTcp.peerPort.toString().c_str());
                }

                transport::RtcSocket tmpSocket(fd, pendingTcp.localPort);
                sendIceErrorResponse(tmpSocket,
                    *msg,
                    pendingTcp.peerPort,
                    ice::StunError::Code::Unauthorized,
                    "Unknown user");
                _allocator.free(packet);
                tmpSocket.detachHandle(); // it must not be closed now
                _epoll.remove(fd, this);
                _pendingConnections.erase(fd);
                return;
            }
        }

        logger::debug("packet received was not valid ICE. closing tcp connection %s",
            _name.c_str(),
            pendingTcp.peerPort.toString().c_str());
        _allocator.free(packet);
        // attack, close the socket
        _epoll.remove(fd, this);
        _pendingConnections.erase(fd);
    }
}

void TcpServerEndpoint::onSocketShutdown(int fd)
{
    if (_pendingConnections.contains(fd))
    {
        _receiveJobs.addJob<EarlyShutdownJob>(*this, fd);
    }
}

void TcpServerEndpoint::internalShutdown(int fd)
{
    auto it = _pendingConnections.find(fd);
    if (it != _pendingConnections.end())
    {
        logger::debug("peer closing tcp connection %s-%s",
            _name.c_str(),
            it->second.localPort.toString().c_str(),
            it->second.peerPort.toString().c_str());
        it->second.packetizer.close();
        _pendingConnections.erase(it->first);
    }
}

void TcpServerEndpoint::internalClosePort(int countDown)
{
    if (countDown > 0)
    {
        for (auto& it : _pendingConnections)
        {
            _epoll.remove(it.second.packetizer.fd, this);
        }
        _pendingConnections.clear();
        if (_pendingEpollRegistrations == 0 && _state == Endpoint::State::CLOSING)
        {
            _receiveJobs.addJob<tcp::ClosePortJob<TcpServerEndpoint>>(*this, 0);
        }
    }
    else
    {
        _state = Endpoint::State::CLOSED;
        _socket.close();
        _listener->onServerPortClosed(*this);
    }
}

void TcpServerEndpoint::internalClosePendingSocket(int fd)
{
    ::close(fd);
    if (--_pendingEpollRegistrations == 0 && _state == Endpoint::State::CLOSING)
    {
        _receiveJobs.addJob<tcp::ClosePortJob<TcpServerEndpoint>>(*this, 0);
    }
}

void TcpServerEndpoint::sendIceErrorResponse(transport::RtcSocket& socket,
    const ice::StunMessage& request,
    const SocketAddress& target,
    int code,
    const char* phrase)
{
    ice::StunMessage response;
    response.header.transactionId = request.header.transactionId;
    response.header.setMethod(ice::StunHeader::BindingErrorResponse);
    response.add(ice::StunXorMappedAddress(target, response.header));
    if (code != 0)
    {
        response.add(ice::StunError(code, phrase));
    }

    response.addFingerprint();

    nwuint16_t shim(response.size());
    socket.sendAggregate(&shim, sizeof(uint16_t), &response, response.size());
}

} // namespace transport
