#pragma once

#include "transport/recp/RecEventBuilder.h"
#include "transport/recp/RecStreamAddedEvent.h"
#include <chrono>

namespace recp
{

class RecStreamAddedEventBuilder final
    : public RecEventBuilder<RecStreamAddedEventBuilder, RecEventType::StreamAdded, RecStreamAddedEvent::MIN_SIZE>
{
public:
    // Using parent constructor
    using TBaseBuilder::RecEventBuilder;

    RecStreamAddedEventBuilder& setSsrc(uint32_t ssrc);
    RecStreamAddedEventBuilder& setIsScreenSharing(bool isScreenSharing);
    RecStreamAddedEventBuilder& setRtpPayloadType(uint8_t payloadType);
    RecStreamAddedEventBuilder& setBridgeCodecNumber(uint8_t codecNumber);
    RecStreamAddedEventBuilder& setEndpoint(const std::string& endpoint);
    RecStreamAddedEventBuilder& setWallClock(std::chrono::system_clock::time_point wallClock);
};

} // namespace recp
