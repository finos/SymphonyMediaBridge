#pragma once
#include "bridge/RtpMap.h"
#include "bridge/engine/SimulcastStream.h"
#include "bridge/engine/SsrcWhitelist.h"
#include "transport/RtcTransport.h"
#include "utils/Optional.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace bridge
{

struct VideoStream
{
    VideoStream(const std::string& id,
        const std::string& endpointId,
        const uint32_t localSsrc,
        std::shared_ptr<transport::RtcTransport>& transport,
        bool ssrcRewrite)
        : _id(id),
          _endpointId(endpointId),
          _endpointIdHash(std::hash<std::string>{}(_endpointId)),
          _localSsrc(localSsrc),
          _simulcastStream{0},
          _transport(transport),
          _markedForDeletion(false),
          _ssrcRewrite(ssrcRewrite),
          _isConfigured(false)
    {
    }

    std::string _id;
    std::string _endpointId;
    size_t _endpointIdHash;
    uint32_t _localSsrc;
    SimulcastStream _simulcastStream;
    utils::Optional<SimulcastStream> _secondarySimulcastStream;
    std::shared_ptr<transport::RtcTransport> _transport;

    bridge::RtpMap _rtpMap;
    bridge::RtpMap _feedbackRtpMap;
    utils::Optional<uint8_t> _absSendTimeExtensionId;

    SsrcWhitelist _ssrcWhitelist;

    bool _markedForDeletion;
    bool _ssrcRewrite;
    bool _isConfigured;
};

} // namespace bridge
