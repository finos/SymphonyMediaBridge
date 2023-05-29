#pragma once

#include <unistd.h>

namespace bridge
{

namespace SsrcRewrite
{

const size_t ssrcArraySize = 32;

}

enum class MediaMode
{
    FORWARD = 0,
    SSRC_REWRITE,
    MIXED,
    LAST
};

} // namespace bridge
