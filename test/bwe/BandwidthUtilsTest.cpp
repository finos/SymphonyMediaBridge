#include "bwe/BandwidthUtils.h"
#include <gtest/gtest.h>

TEST(BandwidthUtilsTest, calcBandwidthFloor)
{
    const auto bandwidthFloor = bwe::BandwidthUtils::calcBandwidthFloor(0, 5, 10, 10);
    printf("bandwidthFloor %u\n", bandwidthFloor);

    const auto simulcastLevel = bwe::BandwidthUtils::calcPinnedHighestSimulcastLevel(0, bandwidthFloor, 2000);
    printf("simulcastLevel %u\n", simulcastLevel);
}
