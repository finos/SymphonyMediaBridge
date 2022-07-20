#pragma once
#include "utils/ContainerAlgorithms.h"
#include <cstdint>
#include <string>
#include <vector>

namespace bridge
{

struct BarbellStreamGroupDescription
{
    BarbellStreamGroupDescription() : slides(false){};

    std::vector<uint32_t> getSsrcs() const
    {
        std::vector<uint32_t> v;
        utils::append(v, ssrcs);
        utils::append(v, feedbackSsrcs);
        return v;
    }

    std::vector<uint32_t> ssrcs;
    std::vector<uint32_t> feedbackSsrcs;
    bool slides;
};

} // namespace bridge
