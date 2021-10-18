#pragma once

#include "transport/recp/RecEventBuilder.h"

namespace recp
{

constexpr uint16_t MIN_STREAM_REMOVED_SIZE = REC_HEADER_SIZE
    + 4; // SSRC

class RecStreamRemovedEventBuilder final
    : public RecEventBuilder<RecStreamRemovedEventBuilder, RecEventType::StreamRemoved, MIN_STREAM_REMOVED_SIZE>
{
public:
    //Using parent constructor
    using TBaseBuilder::RecEventBuilder;

    RecStreamRemovedEventBuilder& setSsrc(uint32_t ssrc);
};

} //namespace recp
