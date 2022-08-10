#pragma once
#include "concurrency/MpmcQueue.h"
#include "ice/IceCandidate.h"
#include "ice/Stun.h"
#include "transport/Endpoint.h"
#include <config/Config.h>
#include <mutex>
#include <unordered_set>

namespace transport
{

class ProbeServer : public Endpoint::IEvents, public ServerEndpoint::IEvents, public Endpoint::IStopEvents
{

public:
    ProbeServer(const ice::IceConfig& iceConfig, const config::Config& config);
    virtual ~ProbeServer(){};

    // Endpoint::IEvents
    virtual void onRtpReceived(Endpoint&,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket) override;

    virtual void onDtlsReceived(Endpoint&,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket) override;

    virtual void onRtcpReceived(Endpoint&,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket) override;

    virtual void onIceReceived(Endpoint&,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket) override;

    virtual void onRegistered(Endpoint&) override;
    virtual void onUnregistered(Endpoint&) override;

    // ServerEndpoint::IEvents
    virtual void onServerPortRegistered(ServerEndpoint&) override;
    virtual void onServerPortUnregistered(ServerEndpoint&) override;

    virtual void onIceTcpConnect(std::shared_ptr<Endpoint>,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket) override;

    // Endpoint::IStopEvents
    virtual void onEndpointStopped(Endpoint*) override;

    // Ice
    const std::pair<std::string, std::string>& getCredentials() const;
    const std::vector<ice::IceCandidate>& getCandidates();

    // Maintenance
    void run();
    void stop();

private:
    std::pair<std::string, std::string> _credentials;
    const ice::IceConfig& _iceConfig;
    const config::Config& _config;
    ice::StunTransactionIdGenerator _idGenerator;
    std::vector<ice::IceCandidate> _iceCandidates;
    std::vector<transport::SocketAddress> _interfaces;

    const char* _name = "ProbeServer";

    struct ProbeTcpConnection
    {
        std::shared_ptr<Endpoint> endpoint;
        uint64_t timestamp;
    };

    std::vector<ProbeTcpConnection> _tcpConnections;
    concurrency::MpmcQueue<ProbeTcpConnection> _queue;
    std::atomic_bool _maintenanceThreadIsRunning;
    std::unique_ptr<std::thread> _maintenanceThread;

    void replyStunOk(Endpoint&, const SocketAddress&, memory::UniquePacket);
    void addCandidate(const ice::IceCandidate& candidate);
    int getInterfaceIndex(transport::SocketAddress address);
};
} // namespace transport
