#pragma once

#include <string>

namespace bridge
{

struct RecordingDescription
{
    std::string _recordingId;
    std::string _ownerId;
    bool _isAudioEnabled;
    bool _isVideoEnabled;
    bool _isScreenSharingEnabled;
};

} // namespace bridge
