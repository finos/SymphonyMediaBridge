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

template <size_t SIZE>
struct SimulcastLevelArray
{
    SimulcastLevelArray() : count(0) {}

    template <size_t N>
    explicit SimulcastLevelArray(SimulcastLevel (&levelsList)[N]) : count(N)
    {
        static_assert(N <= MAX_SIZE, "must not exceed capacity of SimulcastLevelArray");
        for (size_t i = 0; i < N; ++i)
        {
            levels[i] = levelsList[i];
        }
    }

    SimulcastLevelArray(const SimulcastLevelArray& o) { std::memcpy(this, &o, sizeof(o)); };
    SimulcastLevelArray& operator=(const SimulcastLevelArray& o)
    {
        std::memcpy(this, &o, sizeof(o));
        return *this;
    }

    SimulcastLevel levels[SIZE];
    const uint32_t count = 0;
    static const size_t MAX_SIZE = SIZE;
};

using SimulcastGroup = SimulcastLevelArray<3>;

} // namespace bridge
