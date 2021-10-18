#pragma once
#include "utils/ByteOrder.h"
#include <string>

namespace webrtc
{
enum DataChannelPpid : uint32_t
{
    WEBRTC_ESTABLISH = 50,
    WEBRTC_STRING = 51,
    WEBRTC_BINARY = 53,
    WEBRTC_STRING_EMPTY = 56,
    WEBRTC_BINARY_EMPTY = 57
};

enum DataChannelMessageType : uint8_t
{
    DATA_CHANNEL_ACK = 2,
    DATA_CHANNEL_OPEN = 3
};

enum ChannelType : uint8_t
{
    DATA_CHANNEL_RELIABLE = 0,
    DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT = 1, // # rtx
    DATA_CHANNEL_PARTIAL_RELIABLE_TIMED = 2, // lifetime ms
    DATA_CHANNEL_RELIABLE_UNORDERED = 0x80,
    DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED = 0x81, // # RTX
    DATA_CHANNEL_PARTIAL_RELIABLE_TIMED_UNORDERED = 0x82 // lifetime ms
};

// payloadProtocol used for this is 50
class DataChannelOpenMessage
{
public:
    DataChannelOpenMessage() = delete;
    DataChannelOpenMessage(const DataChannelOpenMessage&) = delete;
    DataChannelOpenMessage& operator=(const DataChannelOpenMessage&) = delete;
    static DataChannelOpenMessage& create(void* location, const std::string& label);

    std::string getLabel() const;
    std::string getProtocol() const;
    size_t size() const;
    
    uint8_t messageType = DATA_CHANNEL_OPEN;
    uint8_t channelType;
    nwuint16_t priority;
    nwuint32_t reliability;
    nwuint16_t labelLength;
    nwuint16_t protocolLength;
};

// ACK is sent as one byte

} // namespace webrtc