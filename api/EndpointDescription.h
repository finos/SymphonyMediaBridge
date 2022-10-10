#pragma once

#include "api/RtcDescriptors.h"

namespace api
{

struct EndpointDescription
{
    std::string endpointId;

    utils::Optional<api::Transport> bundleTransport;

    utils::Optional<api::Audio> audio;
    utils::Optional<api::Video> video;
    utils::Optional<api::Data> data;
};

} // namespace api
