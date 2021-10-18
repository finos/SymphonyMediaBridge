#pragma once
#include "concurrency/MpmcQueue.h"
#include "memory/PacketPoolAllocator.h"
#include "test/transport/FakeNetwork.h"
#include "test/transport/NetworkLink.h"
#include "transport/sctp/SctpAssociation.h"
#include "transport/sctp/SctpConfig.h"
#include "transport/sctp/SctpServerPort.h"
#include "transport/sctp/SctpTimer.h"
#include "transport/sctp/Sctprotocol.h"
#include "utils/Time.h"
#include <cassert>
#include <cstdint>
#include <gtest/gtest.h>
#include <inttypes.h>
#include <memory>
#include <random>
#include <thread>
#include <unistd.h>
#include <vector>

namespace sctptest
{

const char* c_str(sctp::ChunkType type);

class SctpEndpoint : public sctp::DatagramTransport,
                     public sctp::SctpServerPort::IEvents,
                     public sctp::SctpAssociation::IEvents
{
    logger::LoggableId _loggableId;

public:
    SctpEndpoint(uint16_t port,
        const sctp::SctpConfig& config,
        uint64_t& timeSource,
        uint32_t bandwidthKbps = 250,
        uint32_t mtu = 1500);
    ~SctpEndpoint();

    void connect(uint16_t dstPort);
    bool forwardPacket(SctpEndpoint& target);
    void forwardPackets(SctpEndpoint& target);
    void forwardWhenReady(SctpEndpoint& target);

    uint64_t process()
    {
        if (!!_session)
        {
            return _session->processTimeout(_timeSource);
        }
        return 5 * sctp::timer::sec;
    }

    uint64_t getTimeout() const;
    size_t getReceivedSize() const { return _dataSizeReceived; }
    size_t getReceivedMessageCount() const { return _receivedMessageCount; }

    std::unique_ptr<sctp::SctpServerPort> _port;
    fakenet::NetworkLink _sendQueue;

    std::unique_ptr<sctp::SctpAssociation> _session;

    uint16_t getStreamId() const { return _streamId; }

    uint32_t sentPacketCount = 0;

    memory::PacketPoolAllocator& getAllocator() { return _allocator; }

private:
    virtual bool sendSctpPacket(const void* data, size_t length) override;

    virtual bool onSctpInitReceived(sctp::SctpServerPort* serverPort,
        uint16_t srcPort,
        const sctp::SctpPacket& sctpPacket,
        uint64_t timestamp,
        uint16_t& inboundStreams,
        uint16_t& outboundStreams) override;

    virtual void onSctpCookieEchoReceived(sctp::SctpServerPort* serverPort,
        uint16_t srcPort,
        const sctp::SctpPacket& packet,
        uint64_t timestamp) override;

    virtual void onSctpReceived(sctp::SctpServerPort* serverPort,
        uint16_t srcPort,
        const sctp::SctpPacket& sctpPacket,
        uint64_t timestamp) override;

    virtual void onSctpStateChanged(sctp::SctpAssociation* session, sctp::SctpAssociation::State state) override;
    virtual void onSctpFragmentReceived(sctp::SctpAssociation* session,
        uint16_t streamId,
        uint16_t streamSequenceNumber,
        uint32_t payloadProtocol,
        const void* buffer,
        size_t length,
        uint64_t timestamp) override
    {
        _dataSizeReceived += length;
        ++_receivedMessageCount;
    }
    virtual void onSctpEstablished(sctp::SctpAssociation* session) override { _streamId = session->allocateStream(); }
    virtual void onSctpClosed(sctp::SctpAssociation* session) override { logger::info("SCTP closed", "SctpEndpoint"); }
    virtual void onSctpChunkDropped(sctp::SctpAssociation* session, size_t size) override
    {
        logger::debug("data chunk droppped %zu", _loggableId.c_str(), size);
    }

    uint16_t _streamId;
    size_t _dataSizeReceived;
    size_t _receivedMessageCount;
    memory::PacketPoolAllocator _allocator;
    const sctp::SctpConfig& _config;
    uint64_t& _timeSource;
    uint32_t _outboundLossCount;
};
} // namespace sctptest
