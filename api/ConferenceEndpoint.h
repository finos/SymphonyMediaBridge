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
};

struct ConferenceEndpointExtendedInfo : public ConferenceEndpoint
{
    std::string ssrc;
    std::string msid;
    std::string localIp;
    std::string localPort;
    std::string protocol;
    std::string remoteIP;
    std::string remotePort;
};
} // namespace api