#pragma once
#include "concurrency/MpmcQueue.h"
#include "crypto/SslHelper.h"
#include "ice/IceCandidate.h"
#include "ice/Stun.h"
#include "jobmanager/JobQueue.h"
#include "transport/Endpoint.h"
#include <config/Config.h>
#include <mutex>
#include <unordered_set>

namespace transport
{

class ProbeServer : public Endpoint::IEvents, public ServerEndpoint::IEvents, public Endpoint::IStopEvents
{

public:
    ProbeServer(const ice::IceConfig& iceConfig, const config::Config& config, jobmanager::JobManager& jobmanager);
    virtual ~ProbeServer() {};

    // Endpoint::IEvents
    void onRtpReceived(Endpoint&,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket,
        uint64_t timestamp) override;

    void onDtlsReceived(Endpoint&,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket,
        uint64_t timestamp) override;

    void onRtcpReceived(Endpoint&,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket,
        uint64_t timestamp) override;

    void onIceReceived(Endpoint&,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket,
        uint64_t timestamp) override;

    void onTcpDisconnect(Endpoint& endpoint) override {}

    void onRegistered(Endpoint&) override;
    void onUnregistered(Endpoint&) override;

    // ServerEndpoint::IEvents
    void onServerPortRegistered(ServerEndpoint&) override;
    void onServerPortUnregistered(ServerEndpoint&) override;

    void onIceTcpConnect(std::shared_ptr<Endpoint>,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket,
        uint64_t timestamp) override;

    // Endpoint::IStopEvents
    void onEndpointStopped(Endpoint*) override;

    // Ice
    const std::pair<std::string, std::string>& getCredentials() const;
    const std::vector<ice::IceCandidate>& getCandidates();

    // Maintenance
    void run();
    void stop();

private:
    void onIceReceivedInternal(Endpoint& endpoint,
        const SocketAddress& source,
        memory::UniquePacket packet,
        uint64_t timestamp);

    void onIceTcpConnectInternal(std::shared_ptr<Endpoint> endpoint,
        const SocketAddress& source,
        memory::UniquePacket packet,
        const uint64_t timestamp);

    bool replyStunOk(Endpoint&, const SocketAddress&, memory::UniquePacket, const uint64_t timestamp);
    void addCandidate(const ice::IceCandidate& candidate);
    int getInterfaceIndex(transport::SocketAddress address);

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

    crypto::HMAC _hmacComputer;
    jobmanager::JobQueue _jobQueue;
    std::vector<ProbeTcpConnection> _tcpConnections;
    concurrency::MpmcQueue<ProbeTcpConnection> _queue;
    std::atomic_bool _maintenanceThreadIsRunning;
    std::unique_ptr<std::thread> _maintenanceThread;
};
} // namespace transport
