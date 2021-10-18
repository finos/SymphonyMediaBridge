#include "DataChannel.h"

namespace webrtc
{
DataChannelOpenMessage& DataChannelOpenMessage::create(void* location, const std::string& label)
{
    auto* m = reinterpret_cast<DataChannelOpenMessage*>(location);

    m->channelType = ChannelType::DATA_CHANNEL_RELIABLE;
    m->labelLength = label.size();
    m->reliability = 0;
    m->protocolLength = 0;

    char* data = reinterpret_cast<char*>(&m->protocolLength + 1);
    std::memcpy(data, label.c_str(), label.size());
    return *m;
}

std::string DataChannelOpenMessage::getLabel() const
{
    const char* data = reinterpret_cast<const char*>(&protocolLength + 1);
    return std::string(data, labelLength);
}

std::string DataChannelOpenMessage::getProtocol() const
{
    const char* data = reinterpret_cast<const char*>(&protocolLength + 1) + labelLength;
    return std::string(data, protocolLength);
}

size_t DataChannelOpenMessage::size() const
{
    return sizeof(DataChannelOpenMessage) + labelLength + protocolLength;
}
} // namespace webrtc