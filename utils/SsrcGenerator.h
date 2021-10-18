#pragma once

#include <cstdint>
#include <random>
#include "MersienneRandom.h"
namespace utils
{

class SsrcGenerator : public MersienneRandom<uint32_t>
{
};

} // namespace utils
