#pragma once
#include "test/transport/NetworkLink.h"
#include "utils/SocketAddress.h"
#include <queue>
namespace fakenet
{

class NetworkNode
{
public:
    virtual void sendTo(const transport::SocketAddress& source,
        const transport::SocketAddress& target,
        const void* data,
        size_t length,
        uint64_t timestamp) = 0;
    virtual bool hasIp(const transport::SocketAddress& target) = 0;
    virtual void process(uint64_t timestamp){};
};

struct Packet
{
    Packet() : length(0) {}
    Packet(const void* data_,
        int length_,
        const transport::SocketAddress& source_,
        const transport::SocketAddress& target_)
        : length(length_),
          source(source_),
          target(target_)
    {
        std::memcpy(data, data_, length);
    }

    uint8_t data[1600];
    size_t length = 0;
    transport::SocketAddress source;
    transport::SocketAddress target;
};

class Gateway : public NetworkNode
{
public:
    virtual void addLocal(NetworkNode* node) = 0;
    virtual void addPublic(NetworkNode* endpoint) = 0;

    virtual void sendTo(const transport::SocketAddress& source,
        const transport::SocketAddress& target,
        const void* data,
        size_t length,
        uint64_t timestamp) override;

protected:
    std::queue<Packet> _packets;
};

class Internet : public Gateway
{
public:
    virtual bool hasIp(const transport::SocketAddress& target) override { return true; }

    void addLocal(NetworkNode* node) override { _nodes.push_back(node); }
    void addPublic(NetworkNode* node) override { _nodes.push_back(node); }
    void process(uint64_t timestamp) override;

private:
    std::vector<NetworkNode*> _nodes;
};
// private nextwork is 172.x.x.x and fe80:....
class Firewall : public Gateway
{
public:
    Firewall(const transport::SocketAddress& publicIp, Gateway& internet);
    ~Firewall() = default;

    void addLocal(NetworkNode* endpoint) override { _endpoints.push_back(endpoint); }
    void addPublic(NetworkNode* endpoint) override { _publicEndpoints.push_back(endpoint); }
    bool hasIp(const transport::SocketAddress& port) override { return _publicInterface.equalsIp(port); }

    transport::SocketAddress getPublicIp() { return _publicInterface; }
    void process(uint64_t timestamp) override;

    bool addPortMapping(const transport::SocketAddress& source, int publicPort);

private:
    void sendToPublic(const transport::SocketAddress& source,
        const transport::SocketAddress& target,
        const void* data,
        size_t len,
        uint64_t timestamp);

    const transport::SocketAddress _publicInterface;
    std::vector<std::pair<transport::SocketAddress, transport::SocketAddress>> _portMappings;
    std::vector<NetworkNode*> _endpoints;
    std::vector<NetworkNode*> _publicEndpoints;
    Gateway& _internet;
    int _portCount = 1000;

    NetworkLink _upLink;
    NetworkLink _downLink;
};
} // namespace fakenet
