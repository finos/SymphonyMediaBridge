#pragma once

#include "UdpEndpoint.h"
#include "concurrency/MpmcQueue.h"
#include "jobmanager/JobQueue.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/Endpoint.h"
#include "transport/RtcSocket.h"
#include "transport/RtcePoll.h"
#include "utils/Trackers.h"

namespace transport
{
class BaseUdpEndpoint : public UdpEndpoint, public RtcePoll::IEventListener
{
public:
    BaseUdpEndpoint(const char* name,
        jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const SocketAddress& localPort,
        RtcePoll& epoll,
        bool isShared);

    void sendTo(const transport::SocketAddress& target, memory::UniquePacket packet) override;

    void registerDefaultListener(IEvents* defaultListener) override;
    void start() override;
    bool openPort(uint16_t port) override;
    void stop(Endpoint::IStopEvents* listener) override;

    SocketAddress getLocalPort() const override { return _socket.getBoundPort(); }

    bool configureBufferSizes(size_t sendBufferSize, size_t receiveBufferSize) override;

    const char* getName() const override { return _name.c_str(); }

    bool isGood() const override { return _socket.isGood(); }

    Endpoint::State getState() const override { return _state; }

    ice::TransportType getTransportType() const override final { return ice::TransportType::UDP; }

    EndpointMetrics getMetrics(uint64_t timestamp) const override final;

private:
    // called on receiveJobs thread
    virtual void internalReceive(int fd, uint32_t batchSize);
    virtual void dispatchReceivedPacket(const SocketAddress& srcAddress,
        memory::UniquePacket packet,
        uint64_t timestamp) = 0;

    virtual void internalStopped();

    // called on sendJobs thread
    virtual void internalSend();

protected:
    std::atomic<Endpoint::State> _state;
    logger::LoggableId _name;
    SocketAddress _localPort;
    RtcSocket _socket;

    void onSocketPollStarted(int fd) override;
    void onSocketPollStopped(int fd) override;
    void onSocketReadable(int fd) override;
    void onSocketShutdown(int fd) override {}
    void onSocketWriteable(int fd) override {}

    struct OutboundPacket
    {
        transport::SocketAddress target;
        memory::UniquePacket packet;
    };

    struct RateMetrics
    {
        RateMetrics() : sendQueueDrops(0) {}
        utils::TrackerWithSnapshot<10, utils::Time::ms * 100, utils::Time::sec> receiveTracker;
        utils::TrackerWithSnapshot<10, utils::Time::ms * 100, utils::Time::sec> sendTracker;
        EndpointMetrics toEndpointMetrics(size_t queueSize) const
        {
            return EndpointMetrics(queueSize,
                receiveTracker.snapshot.load() * 8 * utils::Time::ms,
                sendTracker.snapshot.load() * 8 * utils::Time::ms,
                sendQueueDrops.load());
        }

        std::atomic_uint64_t sendQueueDrops;
    } _rateMetrics;

    jobmanager::JobQueue _receiveJobs;
    jobmanager::JobQueue _sendJobs;
    memory::PacketPoolAllocator& _allocator;
    concurrency::MpmcQueue<OutboundPacket> _sendQueue;

    RtcePoll& _epoll;
    std::atomic_uint32_t _epollCountdown;
    Endpoint::IStopEvents* _stopListener;
    const bool _isShared;
    std::atomic_flag _pendingRead = ATOMIC_FLAG_INIT;
    std::atomic_flag _pendingSend = ATOMIC_FLAG_INIT;
    std::atomic_flag _isFull = ATOMIC_FLAG_INIT;

    std::atomic<IEvents*> _defaultListener;
};
} // namespace transport
