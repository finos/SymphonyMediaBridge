#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace legacyapi
{

struct SsrcAttribute
{
    static const char* slidesContent;
    static const char* videoContent;

    std::vector<uint32_t> _sources;
    std::string _content;
};

} // namespace legacyapi
