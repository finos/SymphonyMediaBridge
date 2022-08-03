#include "EngineStreamDirector.h"

namespace bridge
{
constexpr size_t EngineStreamDirector::configLadder[8][6] = {
    // BasicCost = 0, PinnedQuality, UnpinnedQuality, ExtraCostPerEach, MinCostSanity, MaxCostSanity
    {2500 - 500, highQuality, midQuality, 500, 2500, 6000},
    {500 - 500, midQuality, midQuality, 500, 500, 4000},
    {2500 - 100, highQuality, lowQuality, 100, 2500, 3200},
    {500 - 100, midQuality, lowQuality, 100, 500, 1200},
    {100 - 100, lowQuality, lowQuality, 100, 100, 800},
    {500, midQuality, dropQuality, 0, 500, 500},
    {100, lowQuality, dropQuality, 0, 100, 100},
    {0, dropQuality, dropQuality, 0, 0, 0},
};
} // namespace bridge
