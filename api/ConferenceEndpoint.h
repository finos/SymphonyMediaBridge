#pragma once
#include "transport/dtls/SrtpClient.h"
#include "transport/ice/IceSession.h"
#include <string>

namespace api
{
struct ConferenceEndpoint
{
    std::string id;
    bool isDominantSpeaker;
    bool isActiveTalker;
    ice::IceSession::State iceState;
    transport::SrtpClient::State dtlsState;

    bool operator==(const ConferenceEndpoint& rhs) const
    {
        return rhs.id == id && rhs.isDominantSpeaker == isDominantSpeaker && rhs.isActiveTalker == isActiveTalker &&
            rhs.iceState == iceState && rhs.dtlsState == dtlsState;
    }
};

struct ConferenceEndpointExtendedInfo
{
    uint32_t ssrc;
    uint32_t usid;
    std::string localIP;
    uint16_t localPort;
    std::string protocol;
    std::string remoteIP;
    uint16_t remotePort;
    ConferenceEndpoint basicEndpointInfo;
};
} // namespace api