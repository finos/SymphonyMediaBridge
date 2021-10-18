#pragma once

#include "utils/StringBuilder.h"

namespace legacyapi
{

namespace DataChannelMessage
{

inline void makeEndpointMessage(utils::StringBuilder<2048>& outMessage,
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

inline void makeDominantSpeakerChange(utils::StringBuilder<256>& outMessage, const std::string& endpointId)
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
