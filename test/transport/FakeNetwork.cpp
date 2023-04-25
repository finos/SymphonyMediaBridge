#include "FakeNetwork.h"
#include "concurrency/ThreadUtils.h"
#include "logger/Logger.h"
#include "utils/Time.h"
#include <algorithm>

#define TRACE_FAKENETWORK 0

#if TRACE_FAKENETWORK
#define NETWORK_LOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
#define NETWORK_LOG(fmt, ...)
#endif

namespace fakenet
{
const char* toString(Protocol p)
{
    switch (p)
    {
    case Protocol::UDP:
        return "UDP";
    case Protocol::SYN:
        return "SYN";
    case Protocol::SYN_ACK:
        return "SYN_ACK";
    case Protocol::TCPDATA:
        return "TCP";
    case Protocol::FIN:
        return "FIN";
    case Protocol::ACK:
        return "ACK";
    }

    return "any";
}

Gateway::Gateway() : _packets(4096 * 4) {}

Gateway::~Gateway() {}

void Gateway::onReceive(Protocol protocol,
    const transport::SocketAddress& source,
    const transport::SocketAddress& target,
    const void* data,
    size_t len,
    uint64_t timestamp)
{
    assert(source.getFamily() == target.getFamily());

    const bool pushed = _packets.push(std::make_unique<Packet>(protocol, data, len, source, target));
    if (!pushed)
    {
        logger::warn("gateway queue full", "Fakenetwork");
    }
}

Internet::~Internet()
{
    if (!_packets.empty())
    {
        logger::warn("%zu packets pending in internet", "Internet", _packets.size());
    }
}

void Internet::addLocal(NetworkNode* node)
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    _nodes.push_back(node);
}

void Internet::addPublic(NetworkNode* node)
{
    return addLocal(node);
}

void Internet::removeNode(NetworkNode* node)
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    for (auto it = _nodes.begin(); it != _nodes.end(); ++it)
    {
        if (*it == node)
        {
            _nodes.erase(it);
            return;
        }
    }
}

bool Internet::isPublicPortFree(const transport::SocketAddress& ipPort) const
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    for (auto& node : _nodes)
    {
        if (node->hasIp(ipPort))
        {
            return false;
        }
    }
    return true;
}

void Internet::process(const uint64_t timestamp)
{
    {
        std::lock_guard<std::mutex> lock(_nodesMutex);
        for (auto* node : _nodes)
        {
            node->process(timestamp);
        }
    }
    for (std::unique_ptr<Packet> packet; _packets.pop(packet);)
    {
        std::lock_guard<std::mutex> lock(_nodesMutex);
        for (auto node : _nodes)
        {
            if (node->hasIp(packet->target))
            {
                NETWORK_LOG("forward %s %s -> %s bytes: %lu ",
                    "Internet",
                    toString(packet->protocol),
                    packet->source.toString().c_str(),
                    packet->target.toString().c_str(),
                    packet->length);

                node->onReceive(packet->protocol,
                    packet->source,
                    packet->target,
                    packet->data,
                    packet->length,
                    timestamp);
                break;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(_nodesMutex);
        for (auto* node : _nodes)
        {
            node->process(timestamp);
        }
    }
}

Firewall::Firewall(const transport::SocketAddress& publicIp, Gateway& internet) : _internet(internet)
{
    addPublicIp(publicIp);
    internet.addLocal(this);
}

Firewall::Firewall(const transport::SocketAddress& publicIpv4,
    const transport::SocketAddress& publicIpv6,
    Gateway& internet)
    : _internet(internet)
{
    addPublicIp(publicIpv4);
    addPublicIp(publicIpv6);
    internet.addLocal(this);
}

Firewall::~Firewall()
{
    if (!_packets.empty())
    {
        logger::warn("%zu packets pending in firewall %s, %s",
            "Firewall",
            _packets.size(),
            _publicIpv4.toString().c_str(),
            _publicIpv6.toString().c_str());
    }
}

void Firewall::addLocal(NetworkNode* endpoint)
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    _endpoints.push_back(endpoint);
}

void Firewall::addPublic(NetworkNode* endpoint)
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    _publicEndpoints.push_back(endpoint);
}

void Firewall::removeNode(NetworkNode* node)
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    for (auto it = _endpoints.begin(); it != _endpoints.end(); ++it)
    {
        if (*it == node)
        {
            _endpoints.erase(it);
            return;
        }
    }
    for (auto it = _publicEndpoints.begin(); it != _publicEndpoints.end(); ++it)
    {
        if (*it == node)
        {
            _publicEndpoints.erase(it);
            return;
        }
    }
}

bool Firewall::isLocalPortFree(const transport::SocketAddress& ipPort) const
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    for (auto& node : _endpoints)
    {
        if (node->hasIp(ipPort))
        {
            return false;
        }
    }
    return true;
}

bool Firewall::isPublicPortFree(const transport::SocketAddress& ipPort) const
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    for (auto& node : _publicEndpoints)
    {
        if (node->hasIp(ipPort))
        {
            return false;
        }
    }
    return true;
}

void Firewall::processEndpoints(const uint64_t timestamp)
{
    for (auto* node : _endpoints)
    {
        node->process(timestamp);
    }
    for (auto* node : _publicEndpoints)
    {
        node->process(timestamp);
    }
}

void Firewall::dispatchNAT(const Packet& packet, const uint64_t timestamp)
{
    auto& portMappings = (packet.protocol == Protocol::UDP ? _portMappingsUdp : _portMappingsTcp);
    for (auto mapping : portMappings)
    {
        if (mapping.second == packet.target)
        {
            for (auto endpoint : _endpoints)
            {
                if (endpoint->hasIp(mapping.first))
                {
                    NETWORK_LOG("NAT %s, %s -> %s -> %s",
                        "Firewall",
                        toString(packet.protocol),
                        packet.source.toString().c_str(),
                        packet.target.toString().c_str(),
                        mapping.first.toString().c_str());
                    endpoint->onReceive(packet.protocol,
                        packet.source,
                        mapping.first,
                        packet.data,
                        packet.length,
                        timestamp);
                    return;
                }
            }
        }
    }
}

bool Firewall::dispatchLocally(const Packet& packet, const uint64_t timestamp)
{
    for (auto ep : _endpoints)
    {
        if (ep->hasIp(packet.target))
        {
            NETWORK_LOG("local %s -> %s",
                "Firewall",
                packet.source.toString().c_str(),
                packet.target.toString().c_str());
            ep->onReceive(packet.protocol, packet.source, packet.target, packet.data, packet.length, timestamp);
            return true;
        }
    }

    return false;
}

void Firewall::process(const uint64_t timestamp)
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    processEndpoints(timestamp);
    for (std::unique_ptr<Packet> packet; _packets.pop(packet);)
    {
        assert(!packet->source.empty());
        assert(!packet->target.empty());

        if (hasIp(packet->target))
        {
            dispatchNAT(*packet, timestamp);
            continue;
        }

        if (dispatchLocally(*packet, timestamp))
        {
            continue;
        }

        if (packet->target.getFamily() == AF_INET && (ntohl(packet->target.getIpv4()->sin_addr.s_addr) >> 24 == 172))
        {
            continue;
        }
        else if (packet->target.getFamily() == AF_INET6 &&
            reinterpret_cast<const uint16_t*>(&packet->target.getIpv6()->sin6_addr)[0] == 0xfe80)
        {
            continue;
        }

        packet->source = acquirePortMapping(packet->protocol, packet->source);
        dispatchPublicly(*packet, timestamp);
    }
}

void Firewall::dispatchPublicly(const Packet& packet, const uint64_t timestamp)
{
    assert(packet.source.getFamily() == packet.target.getFamily());
    for (auto publicEp : _publicEndpoints)
    {
        if (publicEp->hasIp(packet.target))
        {
            NETWORK_LOG("dmz %s -> %s", "firewall", packet.source.toString().c_str(), packet.target.toString().c_str());
            publicEp->onReceive(packet.protocol, packet.source, packet.target, packet.data, packet.length, timestamp);
            return;
        }
    }

    _internet.onReceive(packet.protocol, packet.source, packet.target, packet.data, packet.length, timestamp);
}

transport::SocketAddress Firewall::acquirePortMapping(const Protocol protocol, const transport::SocketAddress& source)
{
    auto& portMappings = (protocol == Protocol::UDP ? _portMappingsUdp : _portMappingsTcp);
    for (auto mapping : portMappings)
    {
        if (mapping.first == source)
        {
            return mapping.second;
        }
    }

    while (!addPortMapping(protocol, source, _portCount++)) {}

    return portMappings.back().second;
}

bool Firewall::addPortMapping(const Protocol protocol, const transport::SocketAddress& source, int publicPort)
{
    auto& portMappings = (protocol == Protocol::UDP ? _portMappingsUdp : _portMappingsTcp);
    assert(!source.empty());
    if (portMappings.cend() !=
        std::find_if(portMappings.cbegin(),
            portMappings.cend(),
            [publicPort](const std::pair<const transport::SocketAddress&, const transport::SocketAddress&>& p) {
                return p.second.getPort() == publicPort;
            }))
    {
        return false;
    }
    auto publicAddress = (source.getFamily() == AF_INET6 ? _publicIpv6 : _publicIpv4);

    publicAddress.setPort(publicPort);
    portMappings.push_back(std::make_pair(source, publicAddress));
    logger::info("added NAT %s -> %s", "Firewall", source.toString().c_str(), publicAddress.toString().c_str());
    return true;
}

void Firewall::addPublicIp(const transport::SocketAddress& addr)
{
    if (addr.getFamily() == AF_INET6)
    {
        _publicIpv6 = addr;
    }
    else
    {
        _publicIpv4 = addr;
    }
}

InternetRunner::InternetRunner(const uint64_t interval)
    : _tickInterval(interval),
      _state(State::paused),
      _command(State::paused)
{
    _internet = std::make_shared<Internet>();
    _thread = std::make_unique<std::thread>([this] { this->internetThreadRun(); });
}

InternetRunner::~InternetRunner()
{
    shutdown();
    _thread->join();
}

void InternetRunner::start()
{
    _command = running;
}

void InternetRunner::pause()
{
    _command = paused;
}

void InternetRunner::shutdown()
{
    _command = quit;
}

std::shared_ptr<Internet> InternetRunner::getNetwork()
{
    return _internet;
}

void InternetRunner::internetThreadRun()
{
    concurrency::setThreadName("Internet");
    while (_command != quit)
    {
        if (_command == State::running)
        {
            _state = running;
            _internet->process(utils::Time::getAbsoluteTime());
            utils::Time::nanoSleep(_tickInterval);
        }
        else if (_command == paused)
        {
            _state = paused;
            while (_command == paused)
            {
                // check in to TimeTurner if enabled
                utils::Time::nanoSleep(utils::Time::ms * 10);
            }
        }
    }
}

std::map<std::string, std::shared_ptr<NetworkLink>> getMapOfInternet(std::shared_ptr<Gateway> internet)
{
    std::map<std::string, std::shared_ptr<NetworkLink>> internetMap;
    for (const auto& node : internet->getLocalNodes())
    {
        const auto downlink = node->getDownlink();
        internetMap.emplace(downlink->getName(), downlink);
    }
    for (const auto& node : internet->getPublicNodes())
    {
        const auto downlink = node->getDownlink();
        internetMap.emplace(downlink->getName(), downlink);
    }
    return internetMap;
}
} // namespace fakenet
