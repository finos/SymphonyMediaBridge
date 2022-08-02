#pragma once

#include "utils/StringBuilder.h"

#if ENABLE_LEGACY_API
#include "legacyapi/DataChannelMessage.h"
#endif

namespace api
{

namespace DataChannelMessage
{

inline void makeEndpointMessage(utils::StringBuilder<2048>& outMessage,
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

inline void makeDominantSpeaker(utils::StringBuilder<256>& outMessage, const std::string& endpointId)
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

} // namespace DataChannelMessage

} // namespace api
