#pragma once
#include "NetworkLink.h"
#include "concurrency/MpmcHashmap.h"
#include "concurrency/MpmcQueue.h"
#include "memory/Map.h"
#include "utils/SocketAddress.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace fakenet
{

enum Protocol : uint8_t
{
    UDP = 0,
    SYN,
    SYN_ACK,
    FIN,
    ACK,
    TCPDATA
};

const char* toString(Protocol p);

class NetworkNode
{
public:
    virtual ~NetworkNode() {}

    virtual void onReceive(Protocol protocol,
        const transport::SocketAddress& source,
        const transport::SocketAddress& target,
        const void* data,
        size_t length,
        uint64_t timestamp) = 0;
    virtual bool hasIp(const transport::SocketAddress& target) = 0;
    virtual void process(uint64_t timestamp){};
    virtual std::shared_ptr<fakenet::NetworkLink> getDownlink() { return nullptr; }
};

struct Packet
{
    Packet() : length(0) {}
    Packet(Protocol proto,
        const void* data_,
        int length_,
        const transport::SocketAddress& source_,
        const transport::SocketAddress& target_)
        : length(length_),
          source(source_),
          target(target_),
          protocol(proto)
    {
        std::memcpy(data, data_, length);
    }

    uint8_t data[1600];
    size_t length = 0;
    transport::SocketAddress source;
    transport::SocketAddress target;
    Protocol protocol;
};

class Gateway : public NetworkNode
{
public:
    Gateway();
    ~Gateway();

    virtual void addLocal(NetworkNode* node) = 0;
    virtual void addPublic(NetworkNode* endpoint) = 0;
    virtual std::vector<NetworkNode*>& getLocalNodes() = 0;
    virtual std::vector<NetworkNode*>& getPublicNodes() = 0;

    virtual void removeNode(NetworkNode* node) = 0;

    virtual bool isLocalPortFree(const transport::SocketAddress&) const = 0;
    virtual bool isPublicPortFree(const transport::SocketAddress&) const = 0;

    void onReceive(Protocol protocol,
        const transport::SocketAddress& source,
        const transport::SocketAddress& target,
        const void* data,
        size_t length,
        uint64_t timestamp) override;

protected:
    concurrency::MpmcQueue<std::unique_ptr<Packet>> _packets;
};

class Internet : public Gateway
{
public:
    ~Internet();
    bool hasIp(const transport::SocketAddress& target) override { return true; }

    void addLocal(NetworkNode* node) override;
    void addPublic(NetworkNode* node) override;
    void removeNode(NetworkNode* node) override;

    bool isLocalPortFree(const transport::SocketAddress& ipPort) const override { return isPublicPortFree(ipPort); }
    bool isPublicPortFree(const transport::SocketAddress& ipPort) const override;

    void process(uint64_t timestamp) override;

    std::vector<NetworkNode*>& getLocalNodes() override { return _nodes; };
    std::vector<NetworkNode*>& getPublicNodes() override { return _nodes; };

private:
    mutable std::mutex _nodesMutex;
    std::vector<NetworkNode*> _nodes;
};
// private nextwork is 172.x.x.x and fe80:....
class Firewall : public Gateway
{
public:
    Firewall(const transport::SocketAddress& publicIp, Gateway& internet);
    Firewall(const transport::SocketAddress& publicIpv4, const transport::SocketAddress& publicIpv6, Gateway& internet);
    virtual ~Firewall();

    void addLocal(NetworkNode* endpoint) override;
    void addPublic(NetworkNode* endpoint) override;
    void removeNode(NetworkNode* node) override;

    void addPublicIp(const transport::SocketAddress& addr);

    bool isLocalPortFree(const transport::SocketAddress& ipPort) const override;
    bool isPublicPortFree(const transport::SocketAddress& ipPort) const override;

    bool hasIp(const transport::SocketAddress& port) override
    {
        return _publicIpv4.equalsIp(port) || _publicIpv6.equalsIp(port);
    }

    transport::SocketAddress getPublicIp() const { return _publicIpv4; }
    transport::SocketAddress getPublicIpv6() const { return _publicIpv6; }
    void process(uint64_t timestamp) override;

    transport::SocketAddress addPortMapping(Protocol protocol, const transport::SocketAddress& source, int publicPort);
    void removePortMapping(Protocol protocol, transport::SocketAddress& lanAddress);

    std::vector<NetworkNode*>& getLocalNodes() override { return _endpoints; };
    std::vector<NetworkNode*>& getPublicNodes() override { return _publicEndpoints; };

    void block(const transport::SocketAddress& source, const transport::SocketAddress& destination);
    void unblock(const transport::SocketAddress& source, const transport::SocketAddress& destination);

private:
    void dispatchPublicly(const Packet& packet, uint64_t timestamp);
    void processEndpoints(const uint64_t timestamp);
    void dispatchNAT(const Packet& packet, const uint64_t timestamp);
    bool dispatchLocally(const Packet& packet, const uint64_t timestamp);
    bool isBlackListed(const transport::SocketAddress& source, const transport::SocketAddress& destination);

    transport::SocketAddress acquirePortMapping(Protocol protocol, const transport::SocketAddress& source);

    transport::SocketAddress _publicIpv4;
    transport::SocketAddress _publicIpv6;
    struct PortPair
    {
        transport::SocketAddress lanPort;
        transport::SocketAddress wanPort;
    };

    using PortMap = concurrency::MpmcHashmap32<transport::SocketAddress, PortPair>;

    PortMap _portMappingsUdp;
    PortMap _portMappingsTcp;
    std::vector<NetworkNode*> _endpoints;
    std::vector<NetworkNode*> _publicEndpoints;
    Gateway& _internet;
    int _portCount = 1000;
    mutable std::mutex _nodesMutex;
    memory::Map<std::pair<transport::SocketAddress, transport::SocketAddress>, bool, 1024> _blackList;
};

class InternetRunner
{
public:
    enum State
    {
        running = 1,
        paused,
        quit
    };

    InternetRunner(uint64_t sleepTime);
    ~InternetRunner();
    void start();
    void pause();
    void shutdown();
    std::shared_ptr<Internet> getNetwork();
    bool isRunning() const { return _state == running; };
    bool isPaused() const { return _state == paused; }
    State getState() const { return _state.load(); }

private:
    void internetThreadRun();
    std::shared_ptr<Internet> _internet;
    const uint64_t _tickInterval;
    std::atomic<State> _state;
    std::atomic<State> _command;
    std::unique_ptr<std::thread> _thread;
};

std::map<std::string, std::shared_ptr<NetworkLink>> getMapOfInternet(std::shared_ptr<Gateway> internet);

} // namespace fakenet
