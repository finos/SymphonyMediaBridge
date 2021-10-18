#pragma once

#include <cassert>
#include <cstdint>

namespace ice
{

enum class IceComponent : uint32_t
{
    RTP = 1,
    RTCP = 2
};

inline IceComponent iceComponentFromValue(const uint32_t value)
{
    assert(value == 1 || value == 2);
    return static_cast<IceComponent>(value);
}

} // namespace ice
