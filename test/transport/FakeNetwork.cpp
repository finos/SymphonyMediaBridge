#include "FakeNetwork.h"
#include "logger/Logger.h"

namespace fakenet
{

void Gateway::sendTo(const transport::SocketAddress& source,
    const transport::SocketAddress& target,
    const void* data,
    size_t len,
    uint64_t timestamp)
{
    assert(source.getFamily() == target.getFamily());
    _packets.push(Packet(data, len, source, target));
}

void Internet::process(const uint64_t timestamp)
{
    for (auto* node : _nodes)
    {
        node->process(timestamp);
    }
    while (!_packets.empty())
    {
        auto& packet = _packets.front();
        for (auto node : _nodes)
        {
            if (node->hasIp(packet.target))
            {
                node->sendTo(packet.source, packet.target, packet.data, packet.length, timestamp);
                break;
            }
        }
        _packets.pop();
    }

    for (auto* node : _nodes)
    {
        node->process(timestamp);
    }
}

Firewall::Firewall(const transport::SocketAddress& publicIp, Gateway& internet)
    : _publicInterface(publicIp),
      _internet(internet)
{
    assert(!publicIp.empty());
    internet.addLocal(this);
}

void Firewall::process(const uint64_t timestamp)
{
    while (!_packets.empty())
    {
        Packet packet = _packets.front();
        _packets.pop();
        assert(!packet.source.empty());
        assert(!packet.target.empty());
        assert(packet.source.getFamily() == _publicInterface.getFamily());

        if (_publicInterface.equalsIp(packet.target))
        {
            for (auto mapping : _portMappings)
            {
                if (mapping.second == packet.target)
                {
                    for (auto endpoint : _endpoints)
                    {
                        if (endpoint->hasIp(mapping.first))
                        {
                            logger::info("inbound %s -> %s",
                                "firewall",
                                packet.source.toString().c_str(),
                                packet.target.toString().c_str());
                            endpoint->sendTo(packet.source, mapping.first, packet.data, packet.length, timestamp);
                            return;
                        }
                    }
                    return;
                }
            }
            return;
        }

        for (auto ep : _endpoints)
        {
            if (ep->hasIp(packet.target))
            {
                logger::info("local %s -> %s",
                    "firewall",
                    packet.source.toString().c_str(),
                    packet.target.toString().c_str());
                ep->sendTo(packet.source, packet.target, packet.data, packet.length, timestamp);
                return;
            }
        }
        if (packet.target.getFamily() == AF_INET && (ntohl(packet.target.getIpv4()->sin_addr.s_addr) >> 24 == 172))
        {
            continue;
        }
        else if (packet.target.getFamily() == AF_INET6 &&
            reinterpret_cast<const uint16_t*>(&packet.target.getIpv6()->sin6_addr)[0] == 0xfe80)
        {
            continue;
        }

        for (auto mapping : _portMappings)
        {
            if (mapping.first == packet.source)
            {
                sendToPublic(mapping.second, packet.target, packet.data, packet.length, timestamp);
                return;
            }
        }

        while (!addPortMapping(packet.source, _portCount++)) {}

        sendToPublic(_portMappings.back().second, packet.target, packet.data, packet.length, timestamp);
    }
}

void Firewall::sendToPublic(const transport::SocketAddress& source,
    const transport::SocketAddress& target,
    const void* data,
    size_t len,
    const uint64_t timestamp)
{
    assert(source.getFamily() == target.getFamily());
    for (auto publicEp : _publicEndpoints)
    {
        if (publicEp->hasIp(target))
        {
            logger::info("dmz %s -> %s", "firewall", source.toString().c_str(), target.toString().c_str());
            publicEp->sendTo(source, target, data, len, timestamp);
            return;
        }
    }

    _internet.sendTo(source, target, data, len, timestamp);
}

bool Firewall::addPortMapping(const transport::SocketAddress& source, int publicPort)
{
    assert(!source.empty());
    if (_portMappings.cend() !=
        std::find_if(_portMappings.cbegin(),
            _portMappings.cend(),
            [publicPort](const std::pair<const transport::SocketAddress&, const transport::SocketAddress&>& p) {
                return p.second.getPort() == publicPort;
            }))
    {
        return false;
    }
    auto publicAddress = _publicInterface;
    assert(source.getFamily() == publicAddress.getFamily());
    publicAddress.setPort(publicPort);
    _portMappings.push_back(std::make_pair(source, publicAddress));
    return true;
}
} // namespace fakenet
