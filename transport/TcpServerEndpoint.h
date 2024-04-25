#pragma once

#include "concurrency/MpmcHashmap.h"
#include "transport/TcpEndpointImpl.h"

namespace config
{
class Config;
} // namespace config

namespace transport
{

// "passive" ICE end point
// Used as TCP candidate for remote side to attempt connection to
// On connection attempt:
// ICE request will be read
// if routable to a Transport, a TcpEndpoint will be created and handed over
// to the Transport along with the request. Ownership of TcpEndpoint is transferred.
class TcpServerEndpoint : public ServerEndpoint, public RtcePoll::IEventListener
{
public:
    TcpServerEndpoint(jobmanager::JobManager& jobManager,
        memory::PacketPoolAllocator& allocator,
        RtcePoll& rtcePoll,
        size_t maxPengingSessions,
        TcpEndpointFactory& tcpEndpointFactory,
        const SocketAddress& localPort,
        const config::Config& config);

    virtual ~TcpServerEndpoint();

    void start() override;
    void stop(ServerEndpoint::IStopEvents* listener) override;
    bool isGood() const override { return _socket.isGood(); }

    const SocketAddress getLocalPort() const override { return _socket.getBoundPort(); }

    void registerListener(const std::string& stunUserName, ServerEndpoint::IEvents* listener) override;
    void unregisterListener(const std::string& stunUserName, ServerEndpoint::IEvents* listener) override;

    virtual const char* getName() const override { return _name.c_str(); }

    virtual Endpoint::State getState() const override { return _state; }

    void maintenance(uint64_t timestamp) override;

public: // internal job methods
    void internalUnregisterListener(const std::string& stunUserName, ServerEndpoint::IEvents* listener);
    void internalStopped(tcp::JobContext jobContext);
    void internalReceive(int fd);
    void internalAccept();
    void internalShutdown(int fd);
    void internalClosePendingSocket(int fd);
    void internalMaintenance(uint64_t timestamp);
    void cleanupStaleConnections(uint64_t timestamp);

private:
    void onSocketPollStarted(int fd) override;
    void onSocketPollStopped(int fd) override;
    void onSocketReadable(int fd) override;
    void onSocketShutdown(int fd) override;
    void onSocketWriteable(int fd) override {}

    void acceptNewConnection();

    void sendIceErrorResponse(transport::RtcSocket& socket,
        const ice::StunMessage& request,
        const SocketAddress& target,
        int code,
        const char* phrase);
    struct PendingTcp
    {
        PendingTcp(int fd,
            memory::PacketPoolAllocator& allocator,
            const SocketAddress& localPort,
            const SocketAddress& peerPort);

        RtpDepacketizer packetizer;
        SocketAddress localPort;
        SocketAddress peerPort;
        uint64_t acceptTime;
    };

    std::atomic<Endpoint::State> _state;
    logger::LoggableId _name;
    RtcSocket _socket;
    jobmanager::JobQueue _receiveJobs;
    memory::PacketPoolAllocator& _allocator;
    concurrency::MpmcHashmap32<int, PendingTcp> _pendingConnections;
    uint32_t _pendingEpollRegistrations;
    concurrency::MpmcHashmap32<transport::SocketAddress, uint64_t> _blackList;
    concurrency::MpmcHashmap32<transport::SocketAddress, uint32_t> _pendingConnectCounters;
    concurrency::MpmcHashmap32<std::string, ServerEndpoint::IEvents*> _iceListeners;
    RtcePoll& _epoll;
    std::atomic_uint32_t _epollCountdown;
    ServerEndpoint::IStopEvents* _stopListener;
    const config::Config& _config;
    uint64_t _lastMaintenance;
    TcpEndpointFactory& _tcpEndpointFactory;
};
} // namespace transport
