#pragma once

#include "memory/PoolBuffer.h"
#include "utils/StringBuilder.h"

namespace legacyapi
{

namespace DataChannelMessage
{

template <size_t T>
inline void makeEndpointMessage(utils::StringBuilder<T>& outMessage,
    const std::string& toEndpointId,
    const std::string& fromEndpointId,
    const char* message)
{
    outMessage.append("{\"colibriClass\":\"EndpointMessage\",");
    outMessage.append("\"to\":\"");
    outMessage.append(toEndpointId);
    outMessage.append("\",");
    outMessage.append("\"from\":\"");
    outMessage.append(fromEndpointId);
    outMessage.append("\",");
    outMessage.append("\"msgPayload\":");
    outMessage.append(message);
    outMessage.append("}");
}

inline memory::PoolBuffer<memory::PacketPoolAllocator> makeEndpointMessageBuffer(const std::string& toEndpointId,
    const std::string& fromEndpointId,
    const memory::PoolBuffer<memory::PacketPoolAllocator>& payload)
{
    constexpr const char* TO_STRING = "{\"colibriClass\":\"EndpointMessage\",\"to\":\"";
    constexpr const char* FROM_STRING = "\",\"from\":\"";
    constexpr const char* MSG_STRING = "\",\"msgPayload\":";
    constexpr const char* TAIL_STRING = "}";

    constexpr std::size_t overhead_len = std::char_traits<char>::length(TO_STRING) +
        std::char_traits<char>::length(FROM_STRING) + std::char_traits<char>::length(MSG_STRING) +
        std::char_traits<char>::length(TAIL_STRING);

    const std::size_t extraLen = toEndpointId.length() + fromEndpointId.length() + payload.getLength();
    auto& allocator = payload.getAllocator();
    memory::PoolBuffer<memory::PacketPoolAllocator> buffer(allocator);
    if (!buffer.allocate(overhead_len + extraLen))
    {
        return buffer;
    }

    auto written = buffer.copyFrom(TO_STRING, std::char_traits<char>::length(TO_STRING), 0);
    written += buffer.copyFrom(toEndpointId.c_str(), toEndpointId.length(), written);
    written += buffer.copyFrom(FROM_STRING, std::char_traits<char>::length(FROM_STRING), written);
    written += buffer.copyFrom(fromEndpointId.c_str(), fromEndpointId.length(), written);
    written += buffer.copyFrom(MSG_STRING, std::char_traits<char>::length(MSG_STRING), written);
    written += buffer.copyFrom(payload, written);
    written += buffer.copyFrom(TAIL_STRING, std::char_traits<char>::length(TAIL_STRING), written);

    assert(written == buffer.getLength());
    return buffer;
}

inline void makeDominantSpeakerChange(utils::StringBuilder<256>& outMessage, const char* endpointId)
{
    outMessage.append("{\"colibriClass\":\"DominantSpeakerEndpointChangeEvent\", \"dominantSpeakerEndpoint\":\"");
    outMessage.append(endpointId);
    outMessage.append("\"}");
}

inline void makeLastNEndpointsChangeStart(utils::StringBuilder<1024>& outMessage)
{
    outMessage.append("{\"colibriClass\":\"LastNEndpointsChangeEvent\",\"lastNEndpoints\":[");
}

inline void addUserMediaMapStart(utils::StringBuilder<1024>& outMessage)
{
    outMessage.append("{\"colibriClass\":\"UserMediaMap\",\"endpoints\":[");
}

inline void addUserMediaEndpointStart(utils::StringBuilder<1024>& outMessage, const char* endpointId)
{
    if (!outMessage.endsWidth('['))
    {
        outMessage.append(",");
    }
    outMessage.append("{\"id\":\"");
    outMessage.append(endpointId);
    outMessage.append("\", \"ssrcs\":[");
}

inline void addUserMediaEndpointEnd(utils::StringBuilder<1024>& outMessage)
{
    outMessage.append("]}");
}

inline void addUserMediaSsrc(utils::StringBuilder<1024>& outMessage, uint32_t ssrc)
{
    if (!outMessage.endsWidth('['))
    {
        outMessage.append(",");
    }
    outMessage.append(ssrc);
}

inline void addUserMediaMapEnd(utils::StringBuilder<1024>& outMessage)
{
    outMessage.append("]}");
}

inline void makeLastNEndpointsChangeAppend(utils::StringBuilder<1024>& outMessage,
    const std::string& endpointId,
    const bool isFirst)
{
    if (!isFirst)
    {
        outMessage.append(",");
    }
    outMessage.append("\"");
    outMessage.append(endpointId);
    outMessage.append("\"");
}

inline void makeLastNEndpointsChangeEnd(utils::StringBuilder<1024>& outMessage)
{
    outMessage.append("]}");
}

} // namespace DataChannelMessage

} // namespace legacyapi
