#pragma once

#include <cstdint>
#include <stdio.h>

namespace bridge
{

struct ActiveTalker
{
    ActiveTalker() : endpointHashId(0), isPtt(false), score(0), noiseLevel(0) {}
    ActiveTalker(size_t endpointHashId, bool isPtt, uint8_t score, uint8_t noiseLevel)
        : endpointHashId(endpointHashId),
          isPtt(isPtt),
          score(score),
          noiseLevel(noiseLevel)
    {
    }
    size_t endpointHashId;
    bool isPtt;
    uint8_t score;
    uint8_t noiseLevel;
};

} // namespace bridge
