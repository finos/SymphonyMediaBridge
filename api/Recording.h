#pragma once

#include "api/RecordingChannel.h"
#include <string>
#include <vector>

namespace api
{

struct Recording
{
    std::string _recordingId;
    std::string _userId;
    bool _isAudioEnabled;
    bool _isVideoEnabled;
    bool _isScreenshareEnabled;
    std::vector<RecordingChannel> _channels;
};

} // namespace api
