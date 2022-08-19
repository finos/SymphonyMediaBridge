#pragma once

#include <cstdint>

namespace bridge
{

struct SimulcastLevel
{
    uint32_t ssrc;
    uint32_t feedbackSsrc;
    bool mediaActive;
};

} // namespace bridge
