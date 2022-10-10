#pragma once

#include "api/RecordingChannel.h"
#include <string>
#include <vector>

namespace api
{

struct Recording
{
    std::string recordingId;
    std::string userId;
    bool isAudioEnabled;
    bool isVideoEnabled;
    bool isScreenshareEnabled;
    std::vector<RecordingChannel> channels;
};

} // namespace api
