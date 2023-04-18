#pragma once
#include "concurrency/MpmcHashmap.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/BaseUdpEndpoint.h"

namespace transport
{

/*class RecordingEndpoint
{
public:
    class IRecordingEvents
    {
    public:
        virtual void onRecControlReceived(RecordingEndpoint& endpoint,
            const SocketAddress& source,
            const SocketAddress& target,
            memory::UniquePacket packet) = 0;

        virtual void onUnregistered(RecordingEndpoint& endpoint) = 0;
    };

    virtual void registerRecordingListener(const SocketAddress& remotePort, IRecordingEvents* listener) = 0;

    virtual void unregisterRecordingListener(IRecordingEvents* listener) = 0;
};*/

class RecordingEndpointImpl : public BaseUdpEndpoint
{
public:
    class IRecordingEvents
    {
    public:
        virtual void onRecControlReceived(RecordingEndpointImpl& endpoint,
            const SocketAddress& source,
            const SocketAddress& target,
            memory::UniquePacket packet) = 0;

        virtual void onUnregistered(RecordingEndpointImpl& endpoint) = 0;
    };

    RecordingEndpointImpl(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const SocketAddress& localPort,
        RtcePoll& epoll,
        bool isShared);

    ~RecordingEndpointImpl();

    void sendStunTo(const transport::SocketAddress& target,
        __uint128_t transactionId,
        const void* data,
        size_t len,
        uint64_t timestamp) override
    {
        assert(false);
    };

    void cancelStunTransaction(__uint128_t transactionId) override { assert(false); }

    void registerListener(const std::string& stunUserName, IEvents* listener) override { assert(false); };
    void registerListener(const SocketAddress& remotePort, IEvents* listener) override { assert(false); };

    void unregisterListener(IEvents* listener) override { assert(false); };
    void unregisterListener(const SocketAddress& remotePort, IEvents* listener) override { assert(false); }

    void registerRecordingListener(const SocketAddress& remotePort, IRecordingEvents* listener);

    void unregisterRecordingListener(IRecordingEvents* listener);

public: // internal job interface
    void dispatchReceivedPacket(const SocketAddress& srcAddress,
        memory::UniquePacket packet,
        uint64_t timestamp) override;

    void internalUnregisterListener(IRecordingEvents* listener);

private:
    concurrency::MpmcHashmap32<SocketAddress, IRecordingEvents*> _listeners;
};

typedef RecordingEndpointImpl RecordingEndpoint;
} // namespace transport
