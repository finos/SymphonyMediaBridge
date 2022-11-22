#pragma once
#include "jobmanager/JobQueue.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/Endpoint.h"
#include "transport/RtcSocket.h"
#include "transport/RtcePoll.h"
#include "utils/SocketAddress.h"

namespace transport
{

class RtpDepacketizer
{
public:
    RtpDepacketizer(int fd, memory::PacketPoolAllocator& allocator);

    memory::UniquePacket receive();

    bool isGood() const { return fd != -1 && _streamPrestine; }

    void close();

    int fd;

private:
    nwuint16_t _header;
    size_t _receivedBytes;
    memory::UniquePacket _incompletePacket;
    memory::PacketPoolAllocator& _allocator;
    bool _streamPrestine;
};

namespace tcp
{
template <typename T>
class PortStoppedJob : public jobmanager::Job
{
public:
    PortStoppedJob(T& endpoint, std::atomic_uint32_t& countDown) : _endpoint(endpoint), _countDown(countDown) {}

    void run() override
    {
        auto value = --_countDown;
        if (value == 0)
        {
            _endpoint.internalStopped();
        }
    }

private:
    T& _endpoint;
    std::atomic_uint32_t& _countDown;
};

template <typename T>
class ReceiveJob : public jobmanager::Job
{
public:
    ReceiveJob(T& endpoint, int fd) : _endpoint(endpoint), _fd(fd) {}

    void run() override { _endpoint.internalReceive(_fd); }

private:
    T& _endpoint;
    int _fd;
};
} // namespace tcp

// End point that package ICE, DTLS, RTP in TCP
class TcpEndpoint : public Endpoint, public RtcePoll::IEventListener
{
public:
    TcpEndpoint(jobmanager::JobManager& jobManager,
        memory::PacketPoolAllocator& allocator,
        RtcePoll& epoll,
        int fd,
        const SocketAddress& localPort,
        const SocketAddress& peerPort);

    TcpEndpoint(jobmanager::JobManager& jobManager,
        memory::PacketPoolAllocator& allocator,
        SocketAddress localInterface,
        RtcePoll& epoll);

    virtual ~TcpEndpoint();

    void sendStunTo(const transport::SocketAddress& target,
        __uint128_t transactionId,
        const void* data,
        size_t len,
        uint64_t timestamp) override;

    void sendTo(const transport::SocketAddress& target, memory::UniquePacket packet) override;

    void registerListener(const std::string& stunUserName, IEvents* listener) override;
    void registerListener(const SocketAddress& remotePort, IEvents* listener) override;
    void registerDefaultListener(IEvents* defaultListener) override;

    void unregisterListener(IEvents* listener) override;

    void start() override;
    void stop(Endpoint::IStopEvents* listener) override;

    void connect(const SocketAddress& remotePort);

    SocketAddress getLocalPort() const override { return _socket.getBoundPort(); }
    void cancelStunTransaction(__uint128_t transactionId) override{};

    bool configureBufferSizes(size_t sendBufferSize, size_t receiveBufferSize) override;

    bool isShared() const override { return false; }
    ice::TransportType getTransportType() const override { return ice::TransportType::TCP; }

    const char* getName() const override { return _name.c_str(); }
    Endpoint::State getState() const override { return _state; }

    EndpointMetrics getMetrics(uint64_t timestamp) const override
    {
        return EndpointMetrics(_receiveJobs.getCount(), 0.0, 0.0, 0);
    }

public:
    // internal job interface
    // called on receiveJobs threads
    void internalReceive(int fd);

    // called on sendJobs threads
    void internalSendTo(const transport::SocketAddress& target, memory::UniquePacket packet);
    void continueSend();

    void internalStopped();

private:
    std::atomic<Endpoint::State> _state;
    logger::LoggableId _name;
    RtcSocket _socket;
    RtpDepacketizer _depacketizer;
    SocketAddress _peerPort;
    std::atomic_flag _pendingRead = ATOMIC_FLAG_INIT;

    void onSocketPollStarted(int fd) override;
    void onSocketPollStopped(int fd) override;
    void onSocketReadable(int fd) override;
    void onSocketWriteable(int fd) override;
    void onSocketShutdown(int fd) override;

    void sendPacket(const memory::Packet& packet);

    jobmanager::JobQueue _receiveJobs;
    jobmanager::JobQueue _sendJobs;
    memory::PacketPoolAllocator& _allocator; // only for ICE

    IEvents* _defaultListener;

    std::string _localUser;

    RtcePoll& _epoll;
    std::atomic_uint32_t _epollCountdown;
    Endpoint::IStopEvents* _stopListener;
    memory::UniquePacket _pendingStunRequest;
    memory::Packet _remainder;
};

} // namespace transport
