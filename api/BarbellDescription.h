#pragma once

#include "api/RtcDescriptors.h"

namespace api
{

struct BarbellDescription
{
    std::string barbellId;

    api::Transport transport;

    api::Audio audio;
    api::Video video;
    api::Data data;
};

} // namespace api
