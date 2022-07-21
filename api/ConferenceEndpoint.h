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
} // namespace api