#include "bwe/BandwidthUtils.h"
#include <algorithm>
#include <cassert>

namespace
{

const uint32_t simulcastLevelBandwidthKbps[3] = {100, 500, 2500};
const uint32_t audioBandwidthKbps = 30;

} // namespace

namespace bwe
{

namespace BandwidthUtils
{

uint32_t calcBandwidthFloor(const uint32_t defaultSimulcastLevel,
    const uint32_t lastN,
    const uint32_t audioStreams,
    const uint32_t videoStreams)
{
    if (defaultSimulcastLevel > 2)
    {
        assert(false);
        return 0;
    }

    const auto maxAudioStreams = std::min(audioStreams, lastN);
    const auto maxVideoStreams = std::min(videoStreams, lastN);
    return audioBandwidthKbps * maxAudioStreams + simulcastLevelBandwidthKbps[defaultSimulcastLevel] * maxVideoStreams;
}

uint32_t calcPinnedHighestSimulcastLevel(const uint32_t defaultSimulcastLevel,
    const uint32_t bandwidthFloorKbps,
    const uint32_t uplinkEstimateKbps)
{
    assert(defaultSimulcastLevel <= 2);
    if (defaultSimulcastLevel >= 2)
    {
        return 2;
    }

    for (int32_t i = 2; i >= static_cast<int32_t>(defaultSimulcastLevel); --i)
    {
        if (bandwidthFloorKbps + simulcastLevelBandwidthKbps[i] <= uplinkEstimateKbps)
        {
            return i;
        }
    }

    return defaultSimulcastLevel;
}

uint32_t getSimulcastLevelKbps(const uint32_t simulcastLevel)
{
    if (simulcastLevel > 2)
    {
        assert(false);
        return 0;
    }

    return simulcastLevelBandwidthKbps[simulcastLevel];
}

} // namespace BandwidthUtils

} // namespace bwe
