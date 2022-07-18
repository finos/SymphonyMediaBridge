#include "WebRtcDataStream.h"
#include "logger/Logger.h"
#include "webrtc/DataChannel.h"
#include "webrtc/DataStreamTransport.h"

namespace webrtc
{

WebRtcDataStream::WebRtcDataStream(const size_t logId,
    webrtc::DataStreamTransport& transport,
    memory::PacketPoolAllocator& allocator)
    : _streamId(0),
      _transport(transport),
      _allocator(allocator),
      _state(State::CLOSED)
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

    _transport.sendSctp(_streamId, DataChannelPpid::WEBRTC_ESTABLISH, data, message.size());

    return _streamId;
}

void WebRtcDataStream::sendString(const char* string, const size_t length)
{
    _transport.sendSctp(_streamId, DataChannelPpid::WEBRTC_STRING, string, length);
}

void WebRtcDataStream::sendData(const void* data, size_t length)
{
    _transport.sendSctp(_streamId, DataChannelPpid::WEBRTC_BINARY, data, length);
}

void WebRtcDataStream::onSctpMessage(webrtc::DataStreamTransport* sender,
    uint16_t streamId,
    uint16_t streamSequenceNumber,
    uint32_t payloadProtocol,
    const void* data,
    size_t length)
{
    if (payloadProtocol == DataChannelPpid::WEBRTC_STRING)
    {
        auto cmdData = reinterpret_cast<const char*>(data);
        std::string command(cmdData, length);
        logger::info("received DataChannel %u message %s", "", streamId, command.c_str());
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
            sender->sendSctp(streamId, webrtc::DataChannelPpid::WEBRTC_ESTABLISH, ack, 1);
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

memory::UniquePacket makeUniquePacket(uint16_t streamId,
    uint32_t payloadProtocol,
    const void* message,
    size_t messageSize,
    memory::PacketPoolAllocator& allocator)
{
    assert(sizeof(SctpStreamMessageHeader) + messageSize <= memory::Packet::size);
    if (sizeof(SctpStreamMessageHeader) + messageSize > memory::Packet::size)
    {
        return nullptr;
    }

    auto packet = memory::makeUniquePacket(allocator);
    if (!packet)
    {
        return packet;
    }

    auto* header = reinterpret_cast<SctpStreamMessageHeader*>(packet->get());
    header->id = streamId;
    header->sequenceNumber = 0;
    header->payloadProtocol = payloadProtocol;
    std::memcpy(header->data(), message, messageSize);
    packet->setLength(sizeof(SctpStreamMessageHeader) + messageSize);

    return packet;
}

} // namespace webrtc
