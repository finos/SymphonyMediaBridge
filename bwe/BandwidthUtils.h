#pragma once

#include <cstdint>

namespace bwe
{

namespace BandwidthUtils
{

uint32_t calcBandwidthFloor(const uint32_t defaultSimulcastLevel,
    const uint32_t lastN,
    const uint32_t audioStreams,
    const uint32_t videoStreams);

uint32_t calcPinnedHighestSimulcastLevel(const uint32_t defaultSimulcastLevel,
    const uint32_t bandwidthFloorKbps,
    const uint32_t uplinkEstimateKbps);

uint32_t getSimulcastLevelKbps(const uint32_t simulcastLevel);

} // namespace BandwidthUtils

} // namespace bwe
