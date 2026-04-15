#include "WebRtcDataStream.h"
#include "logger/Logger.h"
#include "webrtc/DataChannel.h"
#include "webrtc/DataStreamTransport.h"
#include "memory/PoolBuffer.h"

namespace webrtc
{

WebRtcDataStream::WebRtcDataStream(const size_t logId, webrtc::DataStreamTransport& transport)
    : _streamId(0),
      _transport(transport),
      _state(State::CLOSED),
      _listener(nullptr)
{
    std::snprintf(_loggableId, sizeof(_loggableId), "WebRtcStream-%zu", logId);
}

uint16_t WebRtcDataStream::open(const std::string& label)
{
    if (_state != State::CLOSED)
    {
        return _streamId;
    }

    _streamId = _transport.allocateOutboundSctpStream();
    char data[label.size() + sizeof(DataChannelOpenMessage)];
    auto& message = DataChannelOpenMessage::create(data, label);
    _state = State::OPENING;

    memory::PoolBuffer<memory::PacketPoolAllocator> buffer(_transport.getAllocator(), data, message.size());
    _transport.sendSctp(_streamId, DataChannelPpid::WEBRTC_ESTABLISH, std::move(buffer));

    return _streamId;
}

void WebRtcDataStream::sendString(const char* string, const size_t length)
{
    memory::PoolBuffer<memory::PacketPoolAllocator> buffer(_transport.getAllocator(), string, length);
    _transport.sendSctp(_streamId, DataChannelPpid::WEBRTC_STRING, std::move(buffer));
}

void WebRtcDataStream::sendData(const void* data, size_t length)
{
    memory::PoolBuffer<memory::PacketPoolAllocator> buffer(_transport.getAllocator(), data, length);
    _transport.sendSctp(_streamId, DataChannelPpid::WEBRTC_BINARY, std::move(buffer));
}

void WebRtcDataStream::sendMessage(uint32_t protocolId, memory::PoolBuffer<memory::PacketPoolAllocator> message) {
    _transport.sendSctp(_streamId, protocolId, std::move(message));
}

void WebRtcDataStream::onSctpMessageBuffer(webrtc::DataStreamTransport* sender,
    const memory::PoolBuffer<memory::PacketPoolAllocator>& message)
{
    // HEADER: SctpStreamMessageHeader prepended to payload
    assert(message.size() >= sizeof(SctpStreamMessageHeader));
    if (message.size() < sizeof(SctpStreamMessageHeader))
    {
        return;
    }
    if (message.size() > 8192) {
        logger::warn("SCTP message too big, len %zu",
            _loggableId,
            message.size()
        );
        return;
    }

    char continousBuffer[message.size()];
    message.copyTo(continousBuffer, 0, message.size());

    const auto& header = *reinterpret_cast<const SctpStreamMessageHeader*>(continousBuffer);
    const auto& payloadProtocol = header.payloadProtocol;
    const auto& streamId = header.id;
    const auto length = message.getLength() - sizeof(SctpStreamMessageHeader);

    if (payloadProtocol == DataChannelPpid::WEBRTC_STRING)
    {
        std::string command(header.getMessage(), length);
        logger::debug("received on stream %u message %s", _loggableId, streamId, command.c_str());
        if (_listener)
        {
            _listener->onWebRtcDataString(header.getMessage(), length);
        }
    }
    if (payloadProtocol != webrtc::DataChannelPpid::WEBRTC_ESTABLISH)
    {
        return;
    }

    if (_state == State::CLOSED)
    {
        auto msg = reinterpret_cast<const webrtc::DataChannelOpenMessage*>(header.data());
        if (msg->messageType == webrtc::DataChannelMessageType::DATA_CHANNEL_OPEN)
        {
            _state = State::OPEN;
            _streamId = streamId;
            _label = msg->getLabel();
            uint8_t ack[] = {webrtc::DATA_CHANNEL_ACK};
            memory::PoolBuffer<memory::PacketPoolAllocator> buffer(_transport.getAllocator(), ack, 1);
            sender->sendSctp(streamId, DataChannelPpid::WEBRTC_ESTABLISH, std::move(buffer));
            logger::info("Data channel open. stream %u", _loggableId, streamId);
        }
    }
    else if (_state == State::OPENING)
    {
        auto* message = reinterpret_cast<const uint8_t*>(header.data());
        if (length > 0 && message[0] == DataChannelMessageType::DATA_CHANNEL_ACK)
        {
            _state = State::OPEN;
            logger::info("Data channel open acknowledged. stream %u", _loggableId, streamId);
        }
    }
}

memory::PoolBuffer<memory::PacketPoolAllocator> makeSctpMessage(uint16_t streamId,
    uint32_t payloadProtocol,
    const void* message,
    size_t messageSize,
    memory::PacketPoolAllocator& allocator)
{
    auto needNullTermination = payloadProtocol == WEBRTC_STRING && message && messageSize > 0 && ((char*)message)[messageSize - 1] == '\0';
    if (payloadProtocol == WEBRTC_STRING_EMPTY || payloadProtocol == WEBRTC_BINARY_EMPTY) {
        needNullTermination = false;
        messageSize = 0;
    }
    const size_t bufferSize = sizeof(SctpStreamMessageHeader) + messageSize + (needNullTermination ? 1 : 0);
    memory::PoolBuffer<memory::PacketPoolAllocator> buffer(allocator);
    if (!buffer.allocate(bufferSize))
    {
        return buffer;
    }

    SctpStreamMessageHeader header = {
        .payloadProtocol = payloadProtocol,
        .id = streamId,
        .sequenceNumber = 0,
    };
    buffer.copyFrom(&header, sizeof(SctpStreamMessageHeader), 0);
    if (messageSize > 0)
    {
        buffer.copyFrom(message, messageSize, sizeof(SctpStreamMessageHeader));
    }

    if (needNullTermination) {
        buffer.copyFrom("\0", 1, sizeof(SctpStreamMessageHeader) + messageSize);
    }

    return buffer;
}
} // namespace webrtc
