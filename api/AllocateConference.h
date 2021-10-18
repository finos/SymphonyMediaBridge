#pragma once

#include "utils/Optional.h"
#include <cstdint>

namespace api
{

struct AllocateConference
{
    utils::Optional<uint32_t> _lastN;
};

} // namespace api
