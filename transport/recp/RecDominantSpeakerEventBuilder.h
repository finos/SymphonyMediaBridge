#pragma once

#include "transport/recp/RecEventBuilder.h"

namespace recp
{

constexpr uint16_t MIN_DOMINANT_SPEAKER_SIZE = REC_HEADER_SIZE + 2;

class RecDominantSpeakerEventBuilder final
    : public RecEventBuilder<RecDominantSpeakerEventBuilder, RecEventType::DominantSpeakerUpdate, MIN_DOMINANT_SPEAKER_SIZE>
{
public:
    //Using parent constructor
    using TBaseBuilder::RecEventBuilder;

    RecDominantSpeakerEventBuilder& setDominantSpeakerEndpoint(const std::string& endpoint);
};

} // namespace recp
