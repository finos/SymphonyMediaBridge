#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace legacyapi
{

struct SsrcGroup
{
    std::vector<uint32_t> _sources;
    std::string _semantics;
};

} // namespace legacyapi
