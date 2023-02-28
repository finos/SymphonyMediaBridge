#include "utils/Neighbours.h"
#include <gtest/gtest.h>

TEST(NeighboursTest, MutuallyNeighbours)
{
    memory::Map<uint32_t, bool, 10> aNeighboursGroups;
    memory::Map<uint32_t, bool, 10> bNeighboursGroups;
    aNeighboursGroups.add(1, true);
    aNeighboursGroups.add(2, true);

    bNeighboursGroups.add(2, true);
    bNeighboursGroups.add(1, true);

    ASSERT_TRUE(areNeighbours(aNeighboursGroups, bNeighboursGroups));
    ASSERT_TRUE(areNeighbours(bNeighboursGroups, aNeighboursGroups));
}
// Scenario "three endpoints in line" (1 in the center)
//  2 --- 1 --- 3
// Middle one (1) does not want to hear neigher 2 nor 3, since they are too close
// But the ones on the edges (2 and 3) want to hear each other.
// So there are two neighbours groups:
// Group id 1 - for endpoitns 1 & 2
// Group id 2 - for endpoints 1 & 3
//
// Consequently, we'll assign  groups like this:
// endpoint 1 belong to groups [1, 2]
// endpont 2 belong to group [1]
// endpoin 3 belong to group [2]

TEST(NeighboursTest, MiddleExcluded)
{
    memory::Map<uint32_t, bool, 10> neighboursOfA;
    memory::Map<uint32_t, bool, 10> neighboursOfB;
    memory::Map<uint32_t, bool, 10> neighboursOfC;
    neighboursOfA.add(1, true);
    neighboursOfA.add(2, true);

    neighboursOfB.add(1, true);

    neighboursOfC.add(2, true);

    ASSERT_TRUE(areNeighbours(neighboursOfB, neighboursOfA));
    ASSERT_TRUE(areNeighbours(neighboursOfA, neighboursOfB));

    ASSERT_TRUE(areNeighbours(neighboursOfC, neighboursOfA));
    ASSERT_TRUE(areNeighbours(neighboursOfA, neighboursOfC));

    ASSERT_FALSE(areNeighbours(neighboursOfB, neighboursOfC));
    ASSERT_FALSE(areNeighbours(neighboursOfC, neighboursOfB));
}