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

    auto buffer = makeUniqueBuffer(_streamId, DataChannelPpid::WEBRTC_ESTABLISH, data, message.size(), _transport.getAllocator());
    _transport.sendSctp(_streamId, DataChannelPpid::WEBRTC_ESTABLISH, std::move(buffer));

    return _streamId;
}

void WebRtcDataStream::sendString(const char* string, const size_t length)
{
    auto buffer = makeUniqueBuffer(_streamId, DataChannelPpid::WEBRTC_STRING, string, length, _transport.getAllocator());
    _transport.sendSctp(_streamId, DataChannelPpid::WEBRTC_STRING, std::move(buffer));
}

void WebRtcDataStream::sendData(const void* data, size_t length)
{
    auto buffer = makeUniqueBuffer(_streamId, DataChannelPpid::WEBRTC_BINARY, data, length, _transport.getAllocator());
    _transport.sendSctp(_streamId, DataChannelPpid::WEBRTC_BINARY, std::move(buffer));
}

void WebRtcDataStream::onSctpMessage(webrtc::DataStreamTransport* sender,
    memory::UniquePoolBuffer<memory::PacketPoolAllocator>& buffer
    )
{
    const auto firstChunk = buffer->getFirstChunk();
    assert(firstChunk.length >= sizeof(SctpStreamMessageHeader));
    const auto& header = *reinterpret_cast<const SctpStreamMessageHeader*>(firstChunk.data);
    const auto& payloadProtocol = header.payloadProtocol;
    const auto& streamId = header.id;
    const auto length = buffer->getLength() - sizeof(SctpStreamMessageHeader);
    const auto data = reinterpret_cast<const uint8_t*>(firstChunk.data) + sizeof(SctpStreamMessageHeader);

    if (payloadProtocol == DataChannelPpid::WEBRTC_STRING)
    {
        std::string command(reinterpret_cast<const char*>(data), length);
        logger::debug("received on stream %u message %s", _loggableId, streamId, command.c_str());
        if (_listener)
        {
            _listener->onWebRtcDataString(reinterpret_cast<const char*>(data), length);
        }
    }
    if (payloadProtocol != webrtc::DataChannelPpid::WEBRTC_ESTABLISH)
    {
        return;
    }

    if (_state == State::CLOSED)
    {
        auto msg = reinterpret_cast<const webrtc::DataChannelOpenMessage*>(data);
        if (msg->messageType == webrtc::DataChannelMessageType::DATA_CHANNEL_OPEN)
        {
            _state = State::OPEN;
            _streamId = streamId;
            _label = msg->getLabel();
            uint8_t ack[] = {webrtc::DATA_CHANNEL_ACK};
            auto buffer = makeUniqueBuffer(_streamId, DataChannelPpid::WEBRTC_ESTABLISH, ack, 1, _transport.getAllocator());
            sender->sendSctp(streamId, DataChannelPpid::WEBRTC_ESTABLISH, std::move(buffer));
            logger::info("Data channel open. stream %u", _loggableId, streamId);
        }
    }
    else if (_state == State::OPENING)
    {
        auto* message = reinterpret_cast<const uint8_t*>(data);
        if (length > 0 && message[0] == DataChannelMessageType::DATA_CHANNEL_ACK)
        {
            _state = State::OPEN;
            logger::info("Data channel open acknowledged. stream %u", _loggableId, streamId);
        }
    }
}

memory::UniquePoolBuffer<memory::PacketPoolAllocator> makeUniqueBuffer(uint16_t streamId,
    uint32_t payloadProtocol,
    const void* message,
    size_t messageSize,
    memory::PacketPoolAllocator& allocator)
{
    auto needNullTermination = !(payloadProtocol == WEBRTC_STRING && (message && messageSize > 0 && ((char*)message)[messageSize - 1] == '\0'));
    if (payloadProtocol == WEBRTC_STRING_EMPTY || payloadProtocol == WEBRTC_BINARY_EMPTY) {
        needNullTermination = false;
        messageSize = 0;
    }
    auto buffer = memory::makeUniquePoolBuffer<memory::PacketPoolAllocator>(allocator, sizeof(SctpStreamMessageHeader) + messageSize + (needNullTermination ? 1 : 0));
    if (!buffer)
    {
        return buffer;
    }

    SctpStreamMessageHeader header = {
        .payloadProtocol = payloadProtocol,
        .id = streamId,
        .sequenceNumber = 0,
    };
    buffer->write(&header, sizeof(SctpStreamMessageHeader), 0);
    buffer->write(message, messageSize, sizeof(SctpStreamMessageHeader));

    if (needNullTermination) {
        buffer->write("\0", 1, sizeof(SctpStreamMessageHeader) + messageSize);
    }

    return buffer;
}
} // namespace webrtc
