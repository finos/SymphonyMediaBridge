#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bridge
{

struct SimulcastLevel
{
    uint32_t _ssrc;
    uint32_t _feedbackSsrc;
    bool _mediaActive;
};

} // namespace bridge
