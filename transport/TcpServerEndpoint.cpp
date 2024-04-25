#include "TcpServerEndpoint.h"
#include "config/Config.h"
#include "ice/Stun.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "utils/Function.h"
#include "utils/Time.h"
#include <arpa/inet.h>
#include <cstdint>
#include <sys/socket.h>

namespace transport
{

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
    TcpEndpointFactory& tcpEndpointFactory,
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
      _config(config),
      _lastMaintenance(0),
      _tcpEndpointFactory(tcpEndpointFactory)
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
    logger::info("server port %s", _name.c_str(), _socket.getBoundPort().toString().c_str());
}

TcpServerEndpoint::~TcpServerEndpoint()
{
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
            stop(nullptr);
        }
    }
}

void TcpServerEndpoint::stop(ServerEndpoint::IStopEvents* listener)
{
    if (_state != Endpoint::State::STOPPING)
    {
        _stopListener = listener;
        _state = Endpoint::State::STOPPING;
        _epoll.remove(_socket.fd(), this);
    }
    else
    {
        if (listener)
        {
            listener->onEndpointStopped(this);
        }
    }
}

void TcpServerEndpoint::maintenance(uint64_t timestamp)
{
    _receiveJobs.post(utils::bind(&TcpServerEndpoint::internalMaintenance, this, timestamp));
}

void TcpServerEndpoint::registerListener(const std::string& stunUserName, ServerEndpoint::IEvents* listener)
{
    if (_iceListeners.contains(stunUserName))
    {
        return;
    }
    _iceListeners.emplace(stunUserName, listener);
    listener->onServerPortRegistered(*this);
}

void TcpServerEndpoint::unregisterListener(const std::string& stunUserName, ServerEndpoint::IEvents* listener)
{
    _receiveJobs.post(utils::bind(&TcpServerEndpoint::internalUnregisterListener, this, stunUserName, listener));
}

void TcpServerEndpoint::internalUnregisterListener(const std::string& stunUserName, ServerEndpoint::IEvents* listener)
{
    if (!_iceListeners.contains(stunUserName))
    {
        return;
    }

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
        _epollCountdown = 1;
        logger::info("server events stopped on %s", _name.c_str(), _socket.getBoundPort().toString().c_str());
        _receiveJobs.addJob<tcp::PortStoppedJob<TcpServerEndpoint, tcp::JobContext::RECEIVE_JOBS>>(*this,
            _epollCountdown);
    }
    else
    {
        _receiveJobs.post([fd]() { ::shutdown(fd, SHUT_RDWR); });
    }
}

void TcpServerEndpoint::onSocketReadable(int fd)
{
    if (fd == _socket.fd())
    {
        _receiveJobs.post(utils::bind(&TcpServerEndpoint::internalAccept, this));
    }
    else
    {
        _receiveJobs.post(utils::bind(&TcpServerEndpoint::internalReceive, this, fd));
    }
}

void TcpServerEndpoint::internalMaintenance(uint64_t timestamp)
{
    if (!_lastMaintenance || utils::Time::diffGE(_lastMaintenance, timestamp, _config.ice.tcp.iceTimeoutSec))
    {
        _lastMaintenance = timestamp;
        cleanupStaleConnections(timestamp);
    }
}

// erase old connection attempts
// If at 90% capacity, identify most frequent peer ip and black list it
void TcpServerEndpoint::cleanupStaleConnections(const uint64_t timestamp)
{
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
    cleanupStaleConnections(utils::Time::getAbsoluteTime());
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
    auto packet = pendingTcp.packetizer.receive();
    if (packet)
    {
        const uint64_t receiveTime = utils::Time::getAbsoluteTime();
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
                        auto endpoint =
                            _tcpEndpointFactory.createTcpEndpoint(fd, pendingTcp.localPort, pendingTcp.peerPort);

                        _pendingConnections.erase(fd);

                        listenIt->second->onIceTcpConnect(endpoint,
                            pendingTcp.peerPort,
                            endpoint->getLocalPort(),
                            std::move(packet),
                            receiveTime

                        );

                        --_pendingEpollRegistrations; // it is not ours anymore

                        logger::debug("ICE request for %s from %s",
                            _name.c_str(),
                            users->getNames().first.c_str(),
                            endpoint->getLocalPort().toString().c_str());

                        _iceListeners.erase(listenIt->first);
                        listenIt->second->onServerPortUnregistered(*this);
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

                tmpSocket.detachHandle(); // fd will be closed in onSocketPollStopped once unreg from ePoll
                _epoll.remove(fd, this);
                _pendingConnections.erase(fd);
                return;
            }
        }

        logger::debug("packet received was not valid ICE. closing tcp connection %s",
            _name.c_str(),
            pendingTcp.peerPort.toString().c_str());

        // attack, close the socket
        _epoll.remove(fd, this);
        _pendingConnections.erase(fd);
    }
}

/**
 * a recently accepted client socket is disconnected from far side before we received anything
 */
void TcpServerEndpoint::onSocketShutdown(int fd)
{
    if (_pendingConnections.contains(fd))
    {
        _receiveJobs.post(utils::bind(&TcpServerEndpoint::internalShutdown, this, fd));
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

void TcpServerEndpoint::internalStopped(tcp::JobContext jobContext)
{
    assert(_epollCountdown == 0);
    _state = Endpoint::State::CREATED;
    if (_stopListener)
    {
        _stopListener->onEndpointStopped(this);
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

    size_t bytesSent = 0;
    nwuint16_t shim(response.size());
    socket.sendAggregate(&shim, sizeof(uint16_t), &response, response.size(), bytesSent);
}

} // namespace transport
