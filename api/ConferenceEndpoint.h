#pragma once
#include "bridge/engine/ActiveTalker.h"
#include "transport/dtls/SrtpClient.h"
#include "transport/ice/IceSession.h"
#include "utils/Optional.h"
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
    bridge::ActiveTalker activeTalkerInfo;

    bool operator==(const ConferenceEndpoint& rhs) const
    {
        return rhs.id == id && rhs.isDominantSpeaker == isDominantSpeaker && rhs.isActiveTalker == isActiveTalker &&
            rhs.iceState == iceState && rhs.dtlsState == dtlsState;
    }
};

struct ConferenceEndpointExtendedInfo
{
    uint32_t ssrcOriginal;
    uint32_t ssrcRewritten;
    utils::Optional<uint32_t> userId;
    std::string localIP;
    uint16_t localPort;
    std::string protocol;
    std::string remoteIP;
    uint16_t remotePort;
    ConferenceEndpoint basicEndpointInfo;
};
} // namespace api