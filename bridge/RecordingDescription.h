#pragma once

#include <string>

namespace bridge
{

struct RecordingDescription
{
    std::string recordingId;
    std::string ownerId;
    bool isAudioEnabled;
    bool isVideoEnabled;
    bool isScreenSharingEnabled;
};

} // namespace bridge
