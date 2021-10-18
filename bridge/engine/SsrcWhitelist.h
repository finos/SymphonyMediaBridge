#pragma once

#include <array>
#include <cstdint>

namespace bridge
{

struct SsrcWhitelist
{
    bool _enabled;
    uint32_t _numSsrcs;
    std::array<uint32_t, 2> _ssrcs;
};

} // namespace bridge
