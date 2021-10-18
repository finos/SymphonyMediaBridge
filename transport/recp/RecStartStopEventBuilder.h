#pragma once

#include "transport/recp/RecEventBuilder.h"

namespace recp
{

constexpr uint16_t MIN_START_STOP_SIZE = REC_HEADER_SIZE + 4;

class RecStartStopEventBuilder final
    : public RecEventBuilder<RecStartStopEventBuilder, RecEventType::StartStop, MIN_START_STOP_SIZE>
{

public:
    // Using parent constructors
    using TBaseBuilder::RecEventBuilder;

    RecStartStopEventBuilder& setAudioEnabled(bool isAudioEnabled);
    RecStartStopEventBuilder& setVideoEnabled(bool isVideoEnabled);
    RecStartStopEventBuilder& setRecordingId(const std::string& recordingId);
    RecStartStopEventBuilder& setUserId(const std::string& userId);
};

} // namespace recp
