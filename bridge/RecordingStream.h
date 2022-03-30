#pragma once

#include "bridge/RecordingDescription.h"
#include "bridge/engine/UnackedPacketsTracker.h"
#include "transport/RecordingTransport.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

namespace bridge
{

struct RecordingStream
{
    RecordingStream(const std::string& id)
        : _id(id),
          _endpointIdHash(std::hash<std::string>{}(id)),
          _audioActiveRecCount(0),
          _videoActiveRecCount(0),
          _screenSharingActiveRecCount(0),
          _markedForDeletion(false)
    {
    }

    std::string _id;
    size_t _endpointIdHash;
    std::unordered_map<size_t, std::unique_ptr<transport::RecordingTransport>> _transports;
    std::unordered_map<size_t, std::unique_ptr<bridge::UnackedPacketsTracker>> _recEventUnackedPacketsTracker;

    uint16_t _audioActiveRecCount;
    uint16_t _videoActiveRecCount;
    uint16_t _screenSharingActiveRecCount;
    std::map<std::string, RecordingDescription> _attachedRecording;

    bool _markedForDeletion;
};

} // namespace bridge
