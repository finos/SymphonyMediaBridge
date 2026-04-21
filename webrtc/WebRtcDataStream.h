#pragma once

#include "memory/PacketPoolAllocator.h"
#include "memory/PoolBuffer.h"
#include <cstdint>
#include <string>

namespace transport
{
class Transport;
} // namespace transport

namespace webrtc
{
class DataStreamTransport;

class WebRtcDataStream
{
public:
    class Listener
    {
    public:
        virtual void onWebRtcDataString(const char* m, size_t len) = 0;
    };

    enum State
    {
        CLOSED = 0,
        OPENING,
        OPEN
    };

    WebRtcDataStream(size_t logId, webrtc::DataStreamTransport& transport);

    uint16_t open(const std::string& label);
    bool isOpen() const { return _state == State::OPEN; }
    void sendString(const char* string, const size_t length);
    void sendData(const void* data, size_t length);
    void sendMessage(uint32_t protocolId, memory::PoolBuffer<memory::PacketPoolAllocator>&& message);

    uint16_t getStreamId() const { return _streamId; };
    std::string getLabel() const { return _label; }

    void onSctpMessageBuffer(webrtc::DataStreamTransport* sender,
        const memory::PoolBuffer<memory::PacketPoolAllocator>& message);

    State getState() const { return _state; }

    void setListener(Listener* listener) { _listener = listener; }

private:
    uint16_t _streamId;
    webrtc::DataStreamTransport& _transport;
    State _state;
    std::string _label;
    char _loggableId[32];
    Listener* _listener;
};

struct SctpStreamMessageHeader
{
    uint32_t payloadProtocol;
    uint16_t id;
    uint16_t sequenceNumber;

    void* data() { return &sequenceNumber + 1; }
    const void* data() const { return &sequenceNumber + 1; }
    const char* getMessage() const { return reinterpret_cast<const char*>(data()); }
    static size_t getMessageLength(size_t packetSize) { return packetSize - sizeof(SctpStreamMessageHeader); }
};
static_assert(sizeof(SctpStreamMessageHeader) == 8, "Misalignment of SctpStreamMessageHeader");

// Creates sctp message with SctpStreamMessageHeader
memory::PoolBuffer<memory::PacketPoolAllocator> makeSctpMessage(uint16_t streamId,
    uint32_t payloadProtocol,
    const void* message,
    size_t messageSize,
    memory::PacketPoolAllocator& allocator);
} // namespace webrtc
