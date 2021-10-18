#pragma once
#include "bridge/RtpMap.h"
#include "transport/RtcTransport.h"
#include "utils/Optional.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace bridge
{

struct AudioStream
{
    AudioStream(const std::string& id,
        const std::string& endpointId,
        const uint32_t localSsrc,
        std::shared_ptr<transport::RtcTransport>& transport,
        const bool audioMixed,
        bool ssrcRewrite)
        : _id(id),
          _endpointId(endpointId),
          _endpointIdHash(std::hash<std::string>{}(_endpointId)),
          _localSsrc(localSsrc),
          _transport(transport),
          _audioLevelExtensionId(-1),
          _audioMixed(audioMixed),
          _rtpMap(),
          _markedForDeletion(false),
          _ssrcRewrite(ssrcRewrite),
          _isConfigured(false)
    {
    }

    std::string _id;
    std::string _endpointId;
    size_t _endpointIdHash;
    uint32_t _localSsrc;
    utils::Optional<uint32_t> _remoteSsrc;

    std::shared_ptr<transport::RtcTransport> _transport;
    int32_t _audioLevelExtensionId;
    bool _audioMixed;

    bridge::RtpMap _rtpMap;

    bool _markedForDeletion;
    bool _ssrcRewrite;
    bool _isConfigured;
};

} // namespace bridge
