#pragma once
#include "concurrency/MpmcQueue.h"
#include "utils/SocketAddress.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

namespace fakenet
{

class NetworkNode
{
public:
    virtual ~NetworkNode() {}

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
    Gateway();
    ~Gateway();

    virtual void addLocal(NetworkNode* node) = 0;
    virtual void addPublic(NetworkNode* endpoint) = 0;

    virtual void sendTo(const transport::SocketAddress& source,
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
    virtual bool hasIp(const transport::SocketAddress& target) override { return true; }

    void addLocal(NetworkNode* node) override
    {
        std::lock_guard<std::mutex> lock(_nodesMutex);
        _nodes.push_back(node);
    }

    void addPublic(NetworkNode* node) override
    {
        std::lock_guard<std::mutex> lock(_nodesMutex);
        _nodes.push_back(node);
    }

    void process(uint64_t timestamp) override;

private:
    mutable std::mutex _nodesMutex;
    std::vector<NetworkNode*> _nodes;
};
// private nextwork is 172.x.x.x and fe80:....
class Firewall : public Gateway
{
public:
    Firewall(const transport::SocketAddress& publicIp, Gateway& internet);
    virtual ~Firewall() = default;

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
    std::shared_ptr<Internet> get();
    bool isRunning() const { return _state == running; };
    bool isPaused() const { return _state == paused; }
    State getState() const { return _state.load(); }

private:
    void internetThreadRun();
    std::shared_ptr<Internet> _internet;
    const uint64_t _sleepTime;
    std::atomic<State> _state;
    std::atomic<State> _command;
    std::unique_ptr<std::thread> _thread;
};
} // namespace fakenet
