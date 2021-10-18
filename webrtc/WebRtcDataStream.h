#pragma once
#include "DataChannel.h"
#include "DataStreamTransport.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/Transport.h"
#include <cstdint>
#include <string>

namespace webrtc
{

class WebRtcDataStream
{
public:
    class IEvents
    {
    public:
        virtual void onWebRtcDataChannelStringMessage(transport::Transport& transport, const std::string&) = 0;
    };

    WebRtcDataStream(size_t logId, webrtc::DataStreamTransport& transport, memory::PacketPoolAllocator& allocator);

    uint16_t open(const std::string& label);
    bool isOpen() const { return _state == State::OPEN; }
    void sendString(const char* string, const size_t length);
    void sendData(const void* data, size_t length);

    uint16_t getStreamId() const { return _streamId; };
    std::string getLabel() const { return _label; }

    static std::string getStringMessage(uint32_t payloadProtocol, const void* data, size_t length);

    void onSctpMessage(webrtc::DataStreamTransport* sender,
        uint16_t streamId,
        uint16_t streamSequenceNumber,
        uint32_t payloadProtocol,
        const void* data,
        size_t length);

private:
    enum State
    {
        CLOSED = 0,
        OPENING,
        OPEN
    };
    uint16_t _streamId;
    webrtc::DataStreamTransport& _transport;
    memory::PacketPoolAllocator& _allocator;
    State _state;
    std::string _label;
    char _loggableId[32];
};

struct SctpStreamMessageHeader
{
    uint32_t payloadProtocol;
    uint16_t id;
    uint16_t sequenceNumber;

    void* data() { return &sequenceNumber + 1; }
    const void* data() const { return &sequenceNumber + 1; }
};

inline const SctpStreamMessageHeader& streamMessageHeader(const memory::Packet* p)
{
    return reinterpret_cast<const SctpStreamMessageHeader&>(*p->get());
}

memory::Packet* makePacket(uint16_t streamId,
    uint32_t payloadProtocol,
    const void* message,
    size_t messageSize,
    memory::PacketPoolAllocator& allocator);
} // namespace webrtc
