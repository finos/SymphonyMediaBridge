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

namespace std
{
template <>
class hash<std::pair<transport::SocketAddress, transport::SocketAddress>>
{
public:
    size_t operator()(const std::pair<transport::SocketAddress, transport::SocketAddress>& addresses) const
    {
        return utils::hash<transport::SocketAddress>()(addresses.first) +
            utils::hash<transport::SocketAddress>()(addresses.second);
    }
};
} // namespace std

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
    case Protocol::ANY:
        return "ANY";
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
        logger::warn("gateway queue full", "FakeNetwork");
    }
}

Internet::~Internet()
{
    if (!_packets.empty())
    {
        logger::warn("%zu packets pending in internet", "Internet", _packets.size());
    }
}

bool Internet::addLocal(NetworkNode* newNode)
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    for (auto& node : _nodes)
    {
        if (node->hasIpClash(*newNode))
        {
            logger::error("IP clash adding public node", "Internet");
            return false;
        }
    }
    _nodes.push_back(newNode);
    return true;
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

bool Internet::isLocalPortFree(const transport::SocketAddress& ipPort, fakenet::Protocol protocol) const
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    for (auto& node : _nodes)
    {
        if (node->hasIp(ipPort, protocol))
        {
            return false;
        }
    }
    return true;
}

bool Internet::hasIpClash(const NetworkNode& newNode) const
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    for (auto& node : _nodes)
    {
        if (node->hasIpClash(newNode))
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
            if (node->hasIp(packet->target, packet->protocol))
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
                packet.reset();
                break;
            }
        }
        if (packet)
        {
            logger::debug("no destination for %s, %zuB, nodes %zu",
                "Internet",
                packet->target.toString().c_str(),
                packet->length,
                _nodes.size());
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

Firewall::Firewall(const transport::SocketAddress& publicIp, Gateway& internet)
    : _portMappingsUdp(512),
      _portMappingsTcp(512),
      _internet(internet),
      _blackList(1024)
{
    addPublicIp(publicIp);
    internet.addLocal(this);
}

Firewall::Firewall(const transport::SocketAddress& publicIpv4,
    const transport::SocketAddress& publicIpv6,
    Gateway& internet)
    : _portMappingsUdp(512),
      _portMappingsTcp(512),
      _internet(internet),
      _blackList(1024)
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

bool Firewall::addLocal(NetworkNode* endpoint)
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    for (auto& node : _endpoints)
    {
        if (node->hasIpClash(*endpoint))
        {
            return false;
        }
    }
    _endpoints.push_back(endpoint);
    return true;
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
}

bool Firewall::hasIpClash(const NetworkNode& newNode) const
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    return newNode.hasIp(_publicIpv4, fakenet::Protocol::UDP) || newNode.hasIp(_publicIpv6, fakenet::Protocol::UDP);
}

bool Firewall::isLocalPortFree(const transport::SocketAddress& ipPort, fakenet::Protocol protocol) const
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    for (auto& node : _endpoints)
    {
        if (node->hasIp(ipPort, protocol))
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
}

void Firewall::dispatchNAT(const Packet& packet, const uint64_t timestamp)
{
    auto& portMap = (packet.protocol == Protocol::UDP ? _portMappingsUdp : _portMappingsTcp);
    auto* portPair = portMap.getItem(packet.target);
    if (portPair && portPair->wanPort == packet.target)
    {
        for (auto endpoint : _endpoints)
        {
            if (endpoint->hasIp(portPair->lanPort, packet.protocol))
            {
                NETWORK_LOG("NAT %s, %s -> %s -> %s",
                    "Firewall",
                    toString(packet.protocol),
                    packet.source.toString().c_str(),
                    packet.target.toString().c_str(),
                    portPair->lanPort.ipToString().c_str());
                endpoint->onReceive(packet.protocol,
                    packet.source,
                    portPair->lanPort,
                    packet.data,
                    packet.length,
                    timestamp);
                return;
            }
        }
    }
}

bool Firewall::dispatchLocally(const Packet& packet, const uint64_t timestamp)
{
    for (auto ep : _endpoints)
    {
        if (ep->hasIp(packet.target, packet.protocol))
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

        if (isBlackListed(packet->source, packet->target))
        {
            continue;
        }

        if (hasIp(packet->target, packet->protocol))
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
        _internet.onReceive(packet->protocol, packet->source, packet->target, packet->data, packet->length, timestamp);
    }
}

transport::SocketAddress Firewall::acquirePortMapping(const Protocol protocol, const transport::SocketAddress& source)
{
    auto& portMap = (protocol == Protocol::UDP ? _portMappingsUdp : _portMappingsTcp);
    auto it = portMap.find(source);
    if (it != portMap.end())
    {
        return it->second.wanPort;
    }

    auto publicAddress = (source.getFamily() == AF_INET6 ? _publicIpv6 : _publicIpv4);
    while (portMap.contains(transport::SocketAddress(publicAddress, ++_portCount)))
    {
    }

    return addPortMapping(protocol, source, _portCount);
}

transport::SocketAddress Firewall::addPortMapping(const Protocol protocol,
    const transport::SocketAddress& source,
    int publicPort)
{
    auto& portMap = (protocol == Protocol::UDP ? _portMappingsUdp : _portMappingsTcp);
    const transport::SocketAddress wanPort(source.getFamily() == AF_INET6 ? _publicIpv6 : _publicIpv4, publicPort);

    assert(!source.empty());
    auto itLan = portMap.find(source);
    if (itLan != portMap.end())
    {
        return itLan->second.wanPort;
    }

    auto itWan = portMap.find(wanPort);
    if (itWan != portMap.end())
    {
        if (source == itWan->second.lanPort)
        {
            return itWan->second.wanPort;
        }
        return transport::SocketAddress();
    }

    PortPair pp{source, wanPort};
    portMap.emplace(source, pp);
    portMap.emplace(wanPort, pp);

    logger::info("added NAT %s -> %s", "Firewall", pp.lanPort.toString().c_str(), pp.wanPort.toString().c_str());
    return pp.wanPort;
}

void Firewall::removePortMapping(Protocol protocol, transport::SocketAddress& lanAddress)
{
    auto& portMap = (protocol == Protocol::UDP ? _portMappingsUdp : _portMappingsTcp);
    auto itLan = portMap.find(lanAddress);
    if (itLan != portMap.end())
    {
        if (portMap.contains(itLan->second.wanPort))
        {
            portMap.erase(itLan->second.wanPort);
        }
        portMap.erase(lanAddress);
    }
}

void Firewall::block(const transport::SocketAddress& source, const transport::SocketAddress& destination)
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    if (isBlackListed(source, destination))
    {
        return;
    }

    _blackList.emplace(std::pair<transport::SocketAddress, transport::SocketAddress>(source, destination));
}

void Firewall::unblock(const transport::SocketAddress& source, const transport::SocketAddress& destination)
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    auto it = _blackList.find(std::pair<transport::SocketAddress, transport::SocketAddress>(source, destination));
    if (it != _blackList.end())
    {
        _blackList.erase(it->first);
        return;
    }
}

bool Firewall::isBlackListed(const transport::SocketAddress& source, const transport::SocketAddress& destination)
{
    if (_blackList.contains(std::pair<transport::SocketAddress, transport::SocketAddress>(source, destination)))
    {
        return true;
    }

    if (source.getFamily() == AF_INET6)
    {
        return _blackList.contains(std::pair<transport::SocketAddress, transport::SocketAddress>(
                   transport::SocketAddress::createBroadcastIpv6(),
                   destination)) ||
            _blackList.contains(std::pair<transport::SocketAddress, transport::SocketAddress>(source,
                transport::SocketAddress::createBroadcastIpv6()));
    }
    else
    {
        return _blackList.contains(std::pair<transport::SocketAddress, transport::SocketAddress>(
                   transport::SocketAddress::createBroadcastIpv4(),
                   destination)) ||
            _blackList.contains(std::pair<transport::SocketAddress, transport::SocketAddress>(source,
                transport::SocketAddress::createBroadcastIpv4()));
    }
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
    logger::info("internet thread started at interval %" PRIu64, "InternetRunner", _tickInterval);
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
            logger::info("internet thread paused...", "InternetRunner");
            _state = paused;
            while (_command == paused)
            {
                // check in to TimeTurner if enabled
                utils::Time::nanoSleep(utils::Time::ms * 10);
            }
            logger::info("internet thread resumed...", "InternetRunner");
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

    return internetMap;
}
} // namespace fakenet
