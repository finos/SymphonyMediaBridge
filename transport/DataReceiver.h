#pragma once

#include "jobmanager/JobQueue.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtpHeader.h"
#include <unistd.h>

namespace transport
{
class RtcTransport;
class RecordingTransport;

class DataReceiver
{
public:
    virtual ~DataReceiver() = default;

    virtual void onRtpPacketReceived(RtcTransport* sender,
        memory::Packet* packet,
        memory::PacketPoolAllocator& receiveAllocator,
        uint32_t extendedSequenceNumber,
        uint64_t timestamp) = 0;

    virtual void onRtcpPacketDecoded(RtcTransport* sender,
        memory::Packet* packet,
        memory::PacketPoolAllocator& receiveAllocator,
        uint64_t timestamp) = 0;

    virtual void onConnected(RtcTransport* sender) = 0;

    virtual bool onSctpConnectionRequest(RtcTransport* sender, uint16_t remotePort) = 0;
    virtual void onSctpEstablished(RtcTransport* sender) = 0;
    virtual void onSctpMessage(RtcTransport* sender,
        uint16_t streamId,
        uint16_t streamSequenceNumber,
        uint32_t payloadProtocol,
        const void* data,
        size_t length) = 0;

    virtual void onRecControlReceived(RecordingTransport* sender,
        memory::Packet* packet,
        memory::PacketPoolAllocator& receiveAllocator,
        uint64_t timestamp) = 0;
};

} // namespace transport
