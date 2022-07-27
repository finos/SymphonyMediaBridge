#pragma once
#include "bridge/BarbellStreamGroupDescription.h"
#include "bridge/RtpMap.h"
#include "bridge/engine/PacketCache.h"
#include "transport/RtcTransport.h"
#include "utils/Optional.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace bridge
{

struct Barbell
{
    Barbell(const std::string& barbellId, std::shared_ptr<transport::RtcTransport>& rtcTransport)
        : id(barbellId),
          transport(rtcTransport),
          markedForDeletion(false),
          isConfigured(false)
    {
    }

    std::string id;

    std::shared_ptr<transport::RtcTransport> transport;
    std::unordered_map<uint32_t, std::unique_ptr<PacketCache>> videoPacketCaches;
    std::vector<BarbellStreamGroupDescription> videoSsrcs;
    std::vector<uint32_t> audioSsrcs;

    RtpMap audioRtpMap;
    RtpMap videoRtpMap;
    RtpMap videoFeedbackRtpMap;

    bool markedForDeletion;
    bool isConfigured;
};

} // namespace bridge
