#pragma once

#include "bridge/CodecCapabilities.h"
#include "utils/Optional.h"
#include <cstdint>

namespace api
{

struct AllocateConference
{
    utils::Optional<uint32_t> lastN;
    bool useGlobalPort = true;
    bridge::VideoCodecSpec videoCodecs = bridge::VideoCodecSpec::makeVp8();
};

} // namespace api
