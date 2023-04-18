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

typedef std::function<void(const SocketAddress& srcAddress, memory::UniquePacket, uint64_t timestamp)> DispatchMethod;

class BaseUdpEndpoint : public RtcePoll::IEventListener
{
public:
    BaseUdpEndpoint(logger::LoggableId& name,
        jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const SocketAddress& localPort,
        RtcePoll& epoll,
        DispatchMethod dispatchMethod,
        Endpoint* endpoint);

    void sendTo(const transport::SocketAddress& target, memory::UniquePacket packet);

    void start();
    bool openPort(uint16_t port);
    void stop(Endpoint::IStopEvents* listener);

    bool configureBufferSizes(size_t sendBufferSize, size_t receiveBufferSize);

    bool isGood() const { return _socket.isGood(); }

    EndpointMetrics getMetrics(uint64_t timestamp) const;

private:
    // called on receiveJobs thread
    virtual void internalReceive(int fd, uint32_t batchSize);

    virtual void internalStopped();

    // called on sendJobs thread
    virtual void internalSend();

    DispatchMethod _dispatchMethod;

public:
    std::atomic<Endpoint::State> _state;
    logger::LoggableId _name;
    SocketAddress _localPort;
    RtcSocket _socket;

private:
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

public:
    jobmanager::JobQueue _receiveJobs;
    jobmanager::JobQueue _sendJobs;
    memory::PacketPoolAllocator& _allocator;
    concurrency::MpmcQueue<OutboundPacket> _sendQueue;

    RtcePoll& _epoll;
    std::atomic_uint32_t _epollCountdown;
    Endpoint::IStopEvents* _stopListener;

    std::atomic_flag _pendingRead = ATOMIC_FLAG_INIT;
    std::atomic_flag _pendingSend = ATOMIC_FLAG_INIT;
    std::atomic_flag _isFull = ATOMIC_FLAG_INIT;

    Endpoint* _parentEndpoint;
};
} // namespace transport
