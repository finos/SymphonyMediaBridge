#pragma once

#include <array>
#include <cstdint>

namespace bridge
{

struct SsrcWhitelist
{
    SsrcWhitelist() : enabled(false), numSsrcs(0), ssrcs{0} {}

    bool enabled;
    uint32_t numSsrcs;
    std::array<uint32_t, 2> ssrcs;
};

} // namespace bridge
