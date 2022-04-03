#include "concurrency/MpmcHashmap.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/BaseUdpEndpoint.h"

namespace transport
{
class RecordingEndpoint : public BaseUdpEndpoint
{
public:
    class IRecordingEvents
    {
    public:
        virtual void onRecControlReceived(RecordingEndpoint& endpoint,
            const SocketAddress& source,
            const SocketAddress& target,
            memory::Packet* packet,
            memory::PacketPoolAllocator& allocator) = 0;

        virtual void onUnregistered(RecordingEndpoint& endpoint) = 0;
    };

    RecordingEndpoint(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const SocketAddress& localPort,
        RtcePoll& epoll,
        bool isShared);

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

    void registerRecordingListener(const SocketAddress& remotePort, IRecordingEvents* listener);

    void unregisterRecordingListener(IRecordingEvents* listener);

public: // internal job interface
    void dispatchReceivedPacket(const SocketAddress& srcAddress, memory::Packet* packet) override;

    void internalUnregisterListener(IRecordingEvents* listener);

private:
    concurrency::MpmcHashmap32<SocketAddress, IRecordingEvents*> _listeners;
};
} // namespace transport
