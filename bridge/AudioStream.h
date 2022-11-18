#pragma once
#include "bridge/RtpMap.h"
#include "transport/RtcTransport.h"
#include "utils/Optional.h"
#include "utils/StdExtensions.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace bridge
{

struct AudioStream
{
    AudioStream(const std::string& id_,
        const std::string& endpointId_,
        const uint32_t localSsrc_,
        std::shared_ptr<transport::RtcTransport>& transport_,
        const bool audioMixed_,
        bool ssrcRewrite_,
        bool isDtlsLocalEnabled_,
        utils::Optional<uint32_t> idleTimeout)
        : id(id_),
          endpointId(endpointId_),
          endpointIdHash(utils::hash<std::string>{}(endpointId)),
          localSsrc(localSsrc_),
          transport(transport_),
          audioMixed(audioMixed_),
          markedForDeletion(false),
          ssrcRewrite(ssrcRewrite_),
          isDtlsLocalEnabled(isDtlsLocalEnabled_),
          isConfigured(false),
          idleTimeoutSeconds(idleTimeout.valueOr(0))
    {
    }

    std::string id;
    std::string endpointId;
    size_t endpointIdHash;
    uint32_t localSsrc;
    utils::Optional<uint32_t> remoteSsrc;

    std::shared_ptr<transport::RtcTransport> transport;
    bool audioMixed;

    bridge::RtpMap rtpMap;

    bool markedForDeletion;
    bool ssrcRewrite;
    bool isDtlsLocalEnabled;
    bool isConfigured;
    const uint32_t idleTimeoutSeconds;
    std::vector<uint32_t> neighbours;
};

} // namespace bridge
