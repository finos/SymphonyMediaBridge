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
    VideoStream(const std::string& id_,
        const std::string& endpointId_,
        const uint32_t localSsrc_,
        std::shared_ptr<transport::RtcTransport>& transport_,
        bool ssrcRewrite_,
        bool isDtlsLocalEnabled_,
        utils::Optional<uint32_t> idleTimeout)
        : id(id_),
          endpointId(endpointId_),
          endpointIdHash(utils::hash<std::string>{}(endpointId)),
          localSsrc(localSsrc_),
          simulcastStream{0},
          transport(transport_),
          markedForDeletion(false),
          ssrcRewrite(ssrcRewrite_),
          isDtlsLocalEnabled(isDtlsLocalEnabled_),
          isConfigured(false),
          idleTimeoutSeconds(idleTimeout.valueOr(0))
    {
    }

    const std::string id;
    const std::string endpointId;
    const size_t endpointIdHash;
    const uint32_t localSsrc;
    SimulcastStream simulcastStream;
    utils::Optional<SimulcastStream> secondarySimulcastStream;
    std::shared_ptr<transport::RtcTransport> transport;

    bridge::RtpMap rtpMap;
    bridge::RtpMap feedbackRtpMap;

    SsrcWhitelist ssrcWhitelist;

    bool markedForDeletion;
    const bool ssrcRewrite;
    const bool isDtlsLocalEnabled;
    bool isConfigured;
    const uint32_t idleTimeoutSeconds;
};

} // namespace bridge
