#include "FakeNetwork.h"
#include "logger/Logger.h"
#include "utils/Time.h"

#define TRACE_FAKENETWORK 0

#if TRACE_FAKENETWORK
#define NETWORK_LOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
#define NETWORK_LOG(fmt, ...)
#endif

namespace fakenet
{

void Gateway::sendTo(const transport::SocketAddress& source,
    const transport::SocketAddress& target,
    const void* data,
    size_t len,
    uint64_t timestamp)
{
    assert(source.getFamily() == target.getFamily());

    NETWORK_LOG("sendTo from: %s  to: %s bytes: %lu",
        "Fakenetwork",
        source.toString().c_str(),
        target.toString().c_str(),
        len);

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
                NETWORK_LOG("process from: %s  to: %s bytes: %lu",
                    "Fakenetwork",
                    packet.source.toString().c_str(),
                    packet.target.toString().c_str(),
                    packet.length);

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

InternetRunner::InternetRunner(const uint64_t sleepTime) : _isRunning(false), _shouldStop(false), _sleepTime(sleepTime)
{
    _internet = std::make_shared<Internet>();
    _thread = std::make_unique<std::thread>([this] { this->internetThreadRun(); });
}

InternetRunner::~InternetRunner()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _shouldStop = true;
        _cv.notify_all();
    }

    _thread->join();
}

void InternetRunner::start()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _isRunning = true;
    _cv.notify_all();
}

void InternetRunner::stop()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _isRunning = false;
    _cv.notify_all();
}

std::shared_ptr<Internet> InternetRunner::get()
{
    return _internet;
}

void InternetRunner::internetThreadRun()
{
    while (true)
    {
        if (_isRunning)
        {
            _internet->process(utils::Time::getAbsoluteTime());
            utils::Time::nanoSleep(_sleepTime);
        }
        else
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _cv.wait(lock, [this] { return _isRunning.load() || _shouldStop.load(); });
            if (_shouldStop)
            {
                _isRunning = false;
                return;
            }
        }
    }
}
} // namespace fakenet
