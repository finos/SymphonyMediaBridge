#pragma once
#include "concurrency/MpmcHashmap.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/BaseUdpEndpoint.h"

namespace transport
{

class RecordingEndpoint : public UdpEndpoint
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
};

class RecordingEndpointImpl : public RecordingEndpoint
{
public:
    RecordingEndpointImpl(jobmanager::JobManager& jobManager,
        size_t maxSessionCount,
        memory::PacketPoolAllocator& allocator,
        const SocketAddress& localPort,
        RtcePoll& epoll,
        bool isShared);

    ~RecordingEndpointImpl();

    void sendStunTo(const transport::SocketAddress& target,
        ice::Int96 transactionId,
        const void* data,
        size_t len,
        uint64_t timestamp) override
    {
        assert(false);
    }

    void cancelStunTransaction(ice::Int96 transactionId) override { assert(false); }

    void registerListener(const std::string& stunUserName, Endpoint::IEvents* listener) override { assert(false); };
    void registerListener(const SocketAddress& remotePort, Endpoint::IEvents* listener) override { assert(false); };

    void unregisterListener(Endpoint::IEvents* listener) override { assert(false); };
    void unregisterListener(const SocketAddress& remotePort, Endpoint::IEvents* listener) override { assert(false); }

    void registerRecordingListener(const SocketAddress& remotePort, IRecordingEvents* listener) override;

    void unregisterRecordingListener(IRecordingEvents* listener) override;

    bool openPort(uint16_t port) override { return _udpEndpoint.openPort(port); }
    bool isGood() const override { return _udpEndpoint.isGood(); }
    ice::TransportType getTransportType() const override { return ice::TransportType::UDP; }
    SocketAddress getLocalPort() const override { return _udpEndpoint._socket.getBoundPort(); }

    void sendTo(const transport::SocketAddress& target, memory::UniquePacket packet) override
    {
        _udpEndpoint.sendTo(target, std::move(packet));
    }

    void registerDefaultListener(IEvents* defaultListener) override{};

    void start() override { _udpEndpoint.start(); }
    void stop(IStopEvents* listener) override { _udpEndpoint.stop(listener); }

    bool configureBufferSizes(size_t sendBufferSize, size_t receiveBufferSize) override
    {
        return _udpEndpoint.configureBufferSizes(sendBufferSize, receiveBufferSize);
    }

    const char* getName() const override { return _name.c_str(); }
    State getState() const override { return _udpEndpoint._state; }

    EndpointMetrics getMetrics(uint64_t timestamp) const override { return _udpEndpoint.getMetrics(timestamp); }

public: // internal job interface
    void dispatchReceivedPacket(const SocketAddress& srcAddress, memory::UniquePacket packet, uint64_t timestamp);

    void internalUnregisterListener(IRecordingEvents* listener);

private:
    logger::LoggableId _name;
    BaseUdpEndpoint _udpEndpoint;
    concurrency::MpmcHashmap32<SocketAddress, IRecordingEvents*> _listeners;
};

} // namespace transport
