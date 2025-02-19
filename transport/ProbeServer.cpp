#include "transport/ProbeServer.h"
#include "concurrency/ThreadUtils.h"
#include "ice/IceCandidate.h"
#include "logger/Logger.h"
#include "transport/ice/IceSession.h"
#include "transport/ice/Stun.h"
#include "utils/ContainerAlgorithms.h"
#include "utils/Function.h"
#include "utils/Time.h"
#include <unistd.h>

namespace transport
{
ProbeServer::ProbeServer(const ice::IceConfig& iceConfig,
    const config::Config& config,
    jobmanager::JobManager& jobmanager)
    : _iceConfig(iceConfig),
      _config(config),
      _jobQueue(jobmanager, 1024),
      _queue(1024),
      _maintenanceThreadIsRunning(true),
      _maintenanceThread(std::make_unique<std::thread>(std::thread([this] { this->run(); })))
{
    char ufrag[14 + 1];
    char pwd[24 + 1]; // length selected to make attribute *4 length

    ice::IceSession::generateCredentialString(_idGenerator, ufrag, sizeof(ufrag) - 1);
    ice::IceSession::generateCredentialString(_idGenerator, pwd, sizeof(pwd) - 1);

    _credentials = std::make_pair<std::string, std::string>(ufrag, pwd);
    _hmacComputer.init(_credentials.second.c_str(), _credentials.second.size());
}

ProbeServer::ProbeServer(const ice::IceConfig& iceConfig,
    const config::Config& config,
    jobmanager::JobManager& jobmanager,
    std::thread&& externalThread)
    : _iceConfig(iceConfig),
      _config(config),
      _jobQueue(jobmanager, 1024),
      _queue(1024),
      _maintenanceThreadIsRunning(true),
      _maintenanceThread(std::make_unique<std::thread>(std::move(externalThread)))
{
    char ufrag[14 + 1];
    char pwd[24 + 1]; // length selected to make attribute *4 length

    ice::IceSession::generateCredentialString(_idGenerator, ufrag, sizeof(ufrag) - 1);
    ice::IceSession::generateCredentialString(_idGenerator, pwd, sizeof(pwd) - 1);

    _credentials = std::make_pair<std::string, std::string>(ufrag, pwd);
    _hmacComputer.init(_credentials.second.c_str(), _credentials.second.size());
}

// Endpoint::IEvents
void ProbeServer::onRtpReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
}

void ProbeServer::onDtlsReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet,
    const uint64_t timestamp) {};

void ProbeServer::onRtcpReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
}

void ProbeServer::onIceReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    _jobQueue.post(utils::bind(&ProbeServer::onIceReceivedInternal,
        this,
        std::ref(endpoint),
        source,
        utils::moveParam(packet),
        timestamp));
}

void ProbeServer::onRegistered(Endpoint& endpoint)
{
    if (endpoint.getTransportType() != ice::TransportType::UDP)
    {
        return;
    }

    const auto localAddress = endpoint.getLocalPort();
    const auto localPort = localAddress.getPort();

    int interfacePreference = 256 - getInterfaceIndex(localAddress);

    addCandidate(ice::IceCandidate(ice::IceComponent::RTP,
        ice::TransportType::UDP,
        ice::IceCandidate::computeCandidatePriority(ice::IceCandidate::Type::HOST,
            interfacePreference,
            ice::IceComponent::RTP,
            ice::TransportType::UDP),
        localAddress,
        localAddress,
        ice::IceCandidate::Type::HOST));

    if (!_iceConfig.publicIpv4.empty() && localAddress.getFamily() == AF_INET)
    {
        auto publicAddress = _iceConfig.publicIpv4;
        publicAddress.setPort(localPort);

        addCandidate(ice::IceCandidate(ice::IceComponent::RTP,
            ice::TransportType::UDP,
            ice::IceCandidate::computeCandidatePriority(ice::IceCandidate::Type::SRFLX,
                interfacePreference,
                ice::IceComponent::RTP,
                ice::TransportType::UDP),
            publicAddress,
            localAddress,
            ice::IceCandidate::Type::SRFLX));

        logger::debug("Registered UDP endpoint %s:%u", _name, publicAddress.ipToString().c_str(), localPort);
    }

    if (!_iceConfig.publicIpv6.empty() && localAddress.getFamily() == AF_INET6)
    {
        auto publicAddress = _iceConfig.publicIpv6;
        publicAddress.setPort(localPort);

        addCandidate(ice::IceCandidate(ice::IceComponent::RTP,
            ice::TransportType::UDP,
            ice::IceCandidate::computeCandidatePriority(ice::IceCandidate::Type::SRFLX,
                interfacePreference,
                ice::IceComponent::RTP,
                ice::TransportType::UDP),
            publicAddress,
            localAddress,
            ice::IceCandidate::Type::SRFLX));

        logger::debug("Registered UDP endpoint %s:%u", _name, publicAddress.ipToString().c_str(), localPort);
    }
}

void ProbeServer::onUnregistered(Endpoint& endpoint)
{
    const SocketAddress& address = endpoint.getLocalPort();

    if (endpoint.getTransportType() == ice::TransportType::UDP)
    {
        auto newEnd =
            std::remove_if(_iceCandidates.begin(), _iceCandidates.end(), [address](const ice::IceCandidate& c) {
                return c.transportType == ice::TransportType::UDP && c.baseAddress == address;
            });

        _iceCandidates.erase(newEnd, _iceCandidates.end());
    }
}

// ServerEndpoint::IEvents
void ProbeServer::onServerPortRegistered(ServerEndpoint& endpoint)
{
    const auto localAddress = endpoint.getLocalPort();
    const auto localPort = localAddress.getPort();

    int interfacePreference = 128 - getInterfaceIndex(localAddress);

    addCandidate(ice::IceCandidate(ice::IceComponent::RTP,
        ice::TransportType::TCP,
        ice::IceCandidate::computeCandidatePriority(ice::IceCandidate::Type::HOST,
            interfacePreference,
            ice::IceComponent::RTP,
            ice::TransportType::TCP),
        localAddress,
        localAddress,
        ice::IceCandidate::Type::HOST,
        ice::TcpType::PASSIVE));

    if (!_iceConfig.publicIpv4.empty() && endpoint.getLocalPort().getFamily() == AF_INET)
    {
        auto publicAddress = _iceConfig.publicIpv4;
        publicAddress.setPort(localPort);

        addCandidate(ice::IceCandidate(ice::IceComponent::RTP,
            ice::TransportType::TCP,
            ice::IceCandidate::computeCandidatePriority(ice::IceCandidate::Type::SRFLX,
                interfacePreference,
                ice::IceComponent::RTP,
                ice::TransportType::TCP),
            publicAddress,
            localAddress,
            ice::IceCandidate::Type::SRFLX));

        logger::debug("Registered TCP server endpoint %s:%u", _name, publicAddress.ipToString().c_str(), localPort);

        if (_config.ice.tcp.aliasPort != 0)
        {
            publicAddress.setPort(_config.ice.tcp.aliasPort);

            addCandidate(ice::IceCandidate(ice::IceComponent::RTP,
                ice::TransportType::TCP,
                ice::IceCandidate::computeCandidatePriority(ice::IceCandidate::Type::SRFLX,
                    interfacePreference,
                    ice::IceComponent::RTP,
                    ice::TransportType::TCP),
                publicAddress,
                localAddress,
                ice::IceCandidate::Type::SRFLX));

            logger::debug("Registered TCP server endpoint %s:%u",
                _name,
                publicAddress.ipToString().c_str(),
                _config.ice.tcp.aliasPort.get());
        }
    }

    if (!_iceConfig.publicIpv6.empty() && endpoint.getLocalPort().getFamily() == AF_INET6)
    {
        auto publicAddress = _iceConfig.publicIpv6;
        publicAddress.setPort(localPort);

        addCandidate(ice::IceCandidate(ice::IceComponent::RTP,
            ice::TransportType::TCP,
            ice::IceCandidate::computeCandidatePriority(ice::IceCandidate::Type::SRFLX,
                interfacePreference,
                ice::IceComponent::RTP,
                ice::TransportType::TCP),
            publicAddress,
            localAddress,
            ice::IceCandidate::Type::SRFLX));

        logger::debug("Registered TCP server endpoint %s:%u", _name, publicAddress.ipToString().c_str(), localPort);

        if (_config.ice.tcp.aliasPort != 0)
        {
            publicAddress.setPort(_config.ice.tcp.aliasPort);

            addCandidate(ice::IceCandidate(ice::IceComponent::RTP,
                ice::TransportType::TCP,
                ice::IceCandidate::computeCandidatePriority(ice::IceCandidate::Type::SRFLX,
                    interfacePreference,
                    ice::IceComponent::RTP,
                    ice::TransportType::TCP),
                publicAddress,
                localAddress,
                ice::IceCandidate::Type::SRFLX));

            logger::debug("Registered TCP server endpoint %s:%u",
                _name,
                publicAddress.ipToString().c_str(),
                _config.ice.tcp.aliasPort.get());
        }
    }
}

void ProbeServer::onServerPortUnregistered(ServerEndpoint& endpoint)
{
    // Re-register to keep listening on probes;
    // TcpServerEndpoint unregisters the listener after dispatching onIceTcpConnect
    endpoint.registerListener(_credentials.first, this);
}

void ProbeServer::onIceTcpConnect(std::shared_ptr<Endpoint> endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{

    _jobQueue.post(utils::bind(&ProbeServer::onIceTcpConnectInternal,
        this,
        endpoint,
        source,
        utils::moveParam(packet),
        timestamp));
}

// Endpoint::IStopEvents
void ProbeServer::onEndpointStopped(Endpoint* endpoint) {}

bool ProbeServer::replyStunOk(Endpoint& endpoint,
    const SocketAddress& destination,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    auto* stunMessage = ice::StunMessage::fromPtr(packet->get());

    if (stunMessage && stunMessage->isValid() && stunMessage->header.isRequest() &&
        stunMessage->isAuthentic(_hmacComputer))
    {
        ice::StunMessage response;
        response.header.transactionId = stunMessage->header.transactionId;
        response.header.setMethod(ice::StunHeader::BindingResponse);
        response.add(ice::StunXorMappedAddress(destination, response.header));
        response.addMessageIntegrity(_hmacComputer);
        response.addFingerprint();

        endpoint.sendStunTo(destination, response.header.transactionId.get(), &response, response.size(), timestamp);
        return true;
    }

    if (endpoint.getTransportType() != ice::TransportType::UDP)
    {
        endpoint.stop(this);
    }

    return false;
}

void ProbeServer::onIceReceivedInternal(Endpoint& endpoint,
    const SocketAddress& source,
    memory::UniquePacket packet,
    uint64_t timestamp)
{
    replyStunOk(endpoint, source, std::move(packet), timestamp);
}

void ProbeServer::onIceTcpConnectInternal(std::shared_ptr<Endpoint> endpoint,
    const SocketAddress& source,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    if (endpoint->getTransportType() == ice::TransportType::TCP)
    {
        if (replyStunOk(*endpoint, source, std::move(packet), timestamp))
        {
            ProbeTcpConnection connection;
            connection.endpoint = endpoint;
            connection.timestamp = utils::Time::getAbsoluteTime();
            _queue.push(connection);
        }
    }
}

void ProbeServer::addCandidate(const ice::IceCandidate& candidate)
{
    if (!utils::contains(_iceCandidates, [candidate](const ice::IceCandidate& x) {
            return x.address == candidate.address && x.baseAddress == candidate.baseAddress &&
                x.transportType == candidate.transportType;
        }))
    {
        _iceCandidates.push_back(candidate);
    }

    std::sort(_iceCandidates.begin(), _iceCandidates.end(), [](const ice::IceCandidate& a, const ice::IceCandidate& b) {
        return a.priority > b.priority;
    });
}

void ProbeServer::run()
{
    concurrency::setThreadName("ProbeServer");

    while (_maintenanceThreadIsRunning)
    {
        while (!_queue.empty())
        {
            ProbeTcpConnection connection;
            if (_queue.pop(connection))
            {
                _tcpConnections.emplace_back(connection);
            }
        }

        auto now = utils::Time::getAbsoluteTime();

        for (auto it = _tcpConnections.begin(); it != _tcpConnections.end();)
        {
            if (utils::Time::diffGT(it->timestamp, now, _iceConfig.probeConnectionExpirationTimeout * utils::Time::sec))
            {
                it->endpoint->stop(this);
                it = _tcpConnections.erase(it);
            }
            else
            {
                ++it;
            }
        }

        utils::Time::nanoSleep(100 * utils::Time::ms);
    }
}

void ProbeServer::stop()
{
    _maintenanceThreadIsRunning = false;
    _maintenanceThread->join();

    while (!_queue.empty())
    {
        ProbeTcpConnection connection;
        if (_queue.pop(connection))
        {
            _tcpConnections.emplace_back(connection);
        }
    }

    for (auto it = _tcpConnections.begin(); it != _tcpConnections.end(); ++it)
    {
        it->endpoint->stop(this);
    }

    _tcpConnections.clear();
}

// ICE
const std::pair<std::string, std::string>& ProbeServer::getCredentials() const
{
    return _credentials;
}

const std::vector<ice::IceCandidate>& ProbeServer::getCandidates()
{
    return _iceCandidates;
}

int ProbeServer::getInterfaceIndex(transport::SocketAddress address)
{
    int index = 0;
    for (auto& interface : _interfaces)
    {
        if (interface == address)
        {
            return index;
        }
        index++;
    }

    _interfaces.emplace_back(address);
    return index;
}

} // namespace transport
