#pragma once

#include <array>
#include <cstdint>

namespace bridge
{

struct SsrcWhitelist
{
    SsrcWhitelist() : enabled(false), numSsrcs(0), ssrcs{0} {}
    SsrcWhitelist& operator=(const SsrcWhitelist& o)
    {
        enabled = o.enabled;
        numSsrcs = o.numSsrcs;
        ssrcs = o.ssrcs;
        return *this;
    }

    bool enabled;
    uint32_t numSsrcs;
    std::array<uint32_t, 2> ssrcs;
};

} // namespace bridge
