
#pragma once

#include <cstdint>
#include <random>

namespace utils
{

template <typename IntType>
class MersienneRandom
{
public:
    MersienneRandom()
        : _generator(reinterpret_cast<std::mt19937_64::result_type>(
              (uint64_t(std::random_device()()) << 32) + std::random_device()())),
          _distribution(0, std::numeric_limits<uint64_t>::max())
    {
    }

    IntType next()
    {
        IntType result = _distribution(_generator);
        for (size_t i = sizeof(IntType); i > sizeof(uint64_t); i -= sizeof(uint64_t))
        {
            for (size_t j = 0; j < sizeof(uint64_t); ++j)
            {
                result <<= 8;
            }
            result |= _distribution(_generator);
        }
        return result;
    }

private:
    std::mt19937_64 _generator;
    std::uniform_int_distribution<uint64_t> _distribution;
};

} // namespace utils
