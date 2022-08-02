#pragma once
#include "api/SimulcastGroup.h"
#include "utils/ContainerAlgorithms.h"
#include <cstdint>
#include <string>
#include <vector>

namespace bridge
{

struct BarbellVideoStreamDescription
{
    BarbellVideoStreamDescription() : slides(false){};

    api::SimulcastGroup ssrcLevels;
    bool slides; // a.k.a screen share
};

} // namespace bridge
