#pragma once
#include "transport/RecordingEndpoint.h"

class FakeRecordingEndpoint : public transport::RecordingEndpoint
{
public:
    FakeRecordingEndpoint() : _name("FakeRecordingEndpoint") {}
    virtual ~FakeRecordingEndpoint() {}

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
    void registerListener(const transport::SocketAddress& remotePort, Endpoint::IEvents* listener) override
    {
        assert(false);
    };

    void unregisterListener(Endpoint::IEvents* listener) override { assert(false); };
    void unregisterListener(const transport::SocketAddress& remotePort, Endpoint::IEvents* listener) override
    {
        assert(false);
    }

    void registerRecordingListener(const transport::SocketAddress& remotePort, IRecordingEvents* listener) override {}

    void unregisterRecordingListener(IRecordingEvents* listener) override {}

    bool openPort(uint16_t port) override { return false; }
    bool isGood() const override { return true; }
    ice::TransportType getTransportType() const override { return ice::TransportType::UDP; }
    transport::SocketAddress getLocalPort() const override { return transport::SocketAddress(); }

    void sendTo(const transport::SocketAddress& target, memory::UniquePacket packet) override {}

    void registerDefaultListener(IEvents* defaultListener) override{};

    void start() override {}
    void stop(IStopEvents* listener) override
    {
        if (listener)
        {
            listener->onEndpointStopped(this);
        }
    }

    bool configureBufferSizes(size_t sendBufferSize, size_t receiveBufferSize) override { return true; }

    const char* getName() const override { return _name.c_str(); }
    State getState() const override { return transport::Endpoint::State::CLOSED; }

    EndpointMetrics getMetrics(uint64_t timestamp) const override { return EndpointMetrics(); }

private:
    logger::LoggableId _name;
};
