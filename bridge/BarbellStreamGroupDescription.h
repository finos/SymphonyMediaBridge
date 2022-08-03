#pragma once
#include "utils/ContainerAlgorithms.h"
#include <cstdint>
#include <string>
#include <vector>

namespace bridge
{

struct BarbellStreamGroupDescription
{
    struct Level
    {
        uint32_t ssrc = 0;
        uint32_t feedbackSsrc = 0;
    };

    BarbellStreamGroupDescription() : slides(false){};

    std::vector<uint32_t> getSsrcs() const
    {
        std::vector<uint32_t> v;
        for (auto& level : ssrcLevels)
        {
            v.push_back(level.ssrc);
            v.push_back(level.feedbackSsrc);
        }
        return v;
    }

    std::vector<Level> ssrcLevels;
    bool slides; // a.k.a screen share
};

} // namespace bridge
