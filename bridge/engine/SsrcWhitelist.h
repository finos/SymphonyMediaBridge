#pragma once

#include <array>
#include <cstdint>

namespace bridge
{

struct SsrcWhitelist
{
    bool enabled;
    uint32_t numSsrcs;
    std::array<uint32_t, 2> ssrcs;
};

} // namespace bridge
