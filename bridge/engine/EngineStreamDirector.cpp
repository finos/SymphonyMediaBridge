#include "EngineStreamDirector.h"

namespace bridge
{
constexpr EngineStreamDirector::ConfigRow EngineStreamDirector::configLadder[6] = {
    // BaseRate = 0, PinnedQuality, UnpinnedQuality, OverheadBitrate, MinBitrateMargin, MaxBitrateMargin
    {2500 - 500, highQuality, midQuality, 500, 2500, 6000},
    {500 - 500, midQuality, midQuality, 500, 500, 4000},
    {500 - 100, midQuality, lowQuality, 100, 500, 1200},
    {100 - 100, lowQuality, lowQuality, 100, 100, 800},
    {100, lowQuality, dropQuality, 0, 100, 100},
    {0, dropQuality, dropQuality, 0, 0, 0}};
} // namespace bridge