#pragma once

#include "memory/PoolBuffer.h"
#include "utils/StringBuilder.h"

#if ENABLE_LEGACY_API
#include "legacyapi/DataChannelMessage.h"
#endif

namespace api
{

namespace DataChannelMessage
{

template <size_t T>
inline void makeEndpointMessage(utils::StringBuilder<T>& outMessage,
    const std::string& toEndpointId,
    const std::string& fromEndpointId,
    const char* message)
{
#if ENABLE_LEGACY_API
    legacyapi::DataChannelMessage::makeEndpointMessage(outMessage, toEndpointId, fromEndpointId, message);
#else
    outMessage.append("{\"type\":\"EndpointMessage\",");
    outMessage.append("\"to\":\"");
    outMessage.append(toEndpointId);
    outMessage.append("\",");
    outMessage.append("\"from\":\"");
    outMessage.append(fromEndpointId);
    outMessage.append("\",");
    outMessage.append("\"payload\":");
    outMessage.append(message);
    outMessage.append("}");
#endif
}

inline memory::UniquePoolBuffer<memory::PacketPoolAllocator> makeUniqueEndpointMessageBuffer(
    const std::string& toEndpointId,
    const std::string& fromEndpointId,
    const memory::UniquePoolBuffer<memory::PacketPoolAllocator>& payload)
{
#if ENABLE_LEGACY_API
    return legacyapi::DataChannelMessage::makeUniqueEndpointMessageBuffer(toEndpointId, fromEndpointId, payload);
#else
    constexpr const char* TO_STRING = "{\"type\":\"EndpointMessage\",\"to\":\"";
    constexpr const char* FROM_STRING = "\",\"from\":\"";
    constexpr const char* MSG_STRING = "\",\"payload\":";
    constexpr const char* TAIL_STRING = "}";

    constexpr std::size_t overhead_len = std::char_traits<char>::length(TO_STRING) + 
        std::char_traits<char>::length(FROM_STRING) + 
        std::char_traits<char>::length(MSG_STRING) + 
        std::char_traits<char>::length(TAIL_STRING);

    const std::size_t extraLen = toEndpointId.length() + fromEndpointId.length() + payload->getLength();
    auto buffer = memory::makeUniquePoolBuffer<memory::PacketPoolAllocator>(payload->getAllocator(), overhead_len + extraLen);
    if (!buffer)
    {
        return buffer;
    }

    auto written = buffer->copyFrom(TO_STRING, std::char_traits<char>::length(TO_STRING), 0);
    written += buffer->copyFrom(toEndpointId.c_str(), toEndpointId.length(), written);
    written += buffer->copyFrom(FROM_STRING, std::char_traits<char>::length(FROM_STRING), written);
    written += buffer->copyFrom(fromEndpointId.c_str(), fromEndpointId.length(), written);
    written += buffer->copyFrom(MSG_STRING, std::char_traits<char>::length(MSG_STRING), written);
    written += buffer->copyFrom(*payload.get(), written);
    written += buffer->copyFrom(TAIL_STRING, std::char_traits<char>::length(TAIL_STRING), written);

    assert(written == buffer->getLength());
    return buffer;
#endif
}

template <size_t T>
inline void makeLoggableStringFromBuffer(memory::Array<char,T>& outArray, memory::UniquePoolBuffer<memory::PacketPoolAllocator>& payload)
{
    if (!payload)
    {
        return;
    }
    outArray.clear();
    bool ellipsisNeeded = payload->getLength() > T - 1;

    const size_t maxCStrLength = std::min(payload->getLength(), T - 1);
    outArray.resize(maxCStrLength + 1);
    const auto read = payload->copyTo(const_cast<void*>(reinterpret_cast<const void*>(outArray.data())), 0, maxCStrLength);
    assert(read == maxCStrLength);
    outArray[maxCStrLength] = '\0';

    // Indicate that message was incompletely logged
    if (ellipsisNeeded && T >= 4)
    {
        outArray[T - 2] = '.';
        outArray[T - 3] = '.';
        outArray[T - 4] = '.';
    }
}

inline void makeDominantSpeaker(utils::StringBuilder<256>& outMessage, const char* endpointId)
{
#if ENABLE_LEGACY_API
    legacyapi::DataChannelMessage::makeDominantSpeakerChange(outMessage, endpointId);
#else
    outMessage.append("{\"type\":\"DominantSpeaker\", \"endpoint\":\"");
    outMessage.append(endpointId);
    outMessage.append("\"}");
#endif
}

inline void makeLastNStart(utils::StringBuilder<1024>& outMessage)
{
#if ENABLE_LEGACY_API
    legacyapi::DataChannelMessage::makeLastNEndpointsChangeStart(outMessage);
#else
    outMessage.append("{\"type\":\"LastN\",\"endpoints\":[");
#endif
}

inline void addUserMediaMapStart(utils::StringBuilder<1024>& outMessage)
{
#if ENABLE_LEGACY_API
    legacyapi::DataChannelMessage::addUserMediaMapStart(outMessage);
#else
    outMessage.append("{\"type\":\"UserMediaMap\",\"endpoints\":[");
#endif
}

inline void addUserMediaEndpointStart(utils::StringBuilder<1024>& outMessage, const char* endpointId)
{
#if ENABLE_LEGACY_API
    legacyapi::DataChannelMessage::addUserMediaEndpointStart(outMessage, endpointId);
#else
    if (!outMessage.endsWidth('['))
    {
        outMessage.append(",");
    }
    outMessage.append("{\"endpoint\":\"");
    outMessage.append(endpointId);
    outMessage.append("\", \"ssrcs\":[");
#endif
}

inline void addUserMediaEndpointEnd(utils::StringBuilder<1024>& outMessage)
{
#if ENABLE_LEGACY_API
    legacyapi::DataChannelMessage::addUserMediaEndpointEnd(outMessage);
#else
    outMessage.append("]}");
#endif
}

inline void addUserMediaSsrc(utils::StringBuilder<1024>& outMessage, uint32_t ssrc)
{
#if ENABLE_LEGACY_API
    legacyapi::DataChannelMessage::addUserMediaSsrc(outMessage, ssrc);
#else
    if (!outMessage.endsWidth('['))
    {
        outMessage.append(",");
    }
    outMessage.append(ssrc);
#endif
}

inline void addUserMediaMapEnd(utils::StringBuilder<1024>& outMessage)
{
#if ENABLE_LEGACY_API
    legacyapi::DataChannelMessage::addUserMediaMapEnd(outMessage);
#else
    outMessage.append("]}");
#endif
}

inline void makeLastNAppend(utils::StringBuilder<1024>& outMessage, const char* endpointId, const bool isFirst)
{
#if ENABLE_LEGACY_API
    legacyapi::DataChannelMessage::makeLastNEndpointsChangeAppend(outMessage, endpointId, isFirst);
#else
    if (!isFirst)
    {
        outMessage.append(",");
    }
    outMessage.append("\"");
    outMessage.append(endpointId);
    outMessage.append("\"");
#endif
}

inline void makeLastNEnd(utils::StringBuilder<1024>& outMessage)
{
#if ENABLE_LEGACY_API
    legacyapi::DataChannelMessage::makeLastNEndpointsChangeEnd(outMessage);
#else
    outMessage.append("]}");
#endif
}

inline void makeMinUplinkBitrate(utils::StringBuilder<1024>& outMessage, const uint32_t maxBitrateKbps)
{
    outMessage.append(R"({"type":"min-uplink-bitrate", "bitrateKbps":)");
    outMessage.append(maxBitrateKbps);
    outMessage.append(R"(})");
}

} // namespace DataChannelMessage

} // namespace api
