#pragma once

#include "MersienneRandom.h"
#include <cstdint>
#include <random>
namespace utils
{

class IdGenerator : public utils::MersienneRandom<uint64_t>
{
};

} // namespace utils
