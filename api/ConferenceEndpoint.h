#pragma once
#include "transport/dtls/SrtpClient.h"
#include "transport/ice/IceSession.h"
#include <string>

namespace api
{
struct ConferenceEndpoint
{
    std::string id;
    bool isBundled;
    bool hasAudio;
    bool hasVideo;
    bool isActiveSpeaker;
    bool isRecording;
    ice::IceSession::State iceState;
    transport::SrtpClient::State dtlsState;
};
} // namespace api