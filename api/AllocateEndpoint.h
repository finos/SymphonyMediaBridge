#pragma once

#include "api/RtcDescriptors.h"
#include "transport/dtls/SrtpProfiles.h"
#include "utils/Optional.h"
#include <cstdint>
#include <string>

namespace api
{

struct AllocateEndpoint
{
    struct Transport
    {
        Transport() : ice(false), srtpMode(SrtpMode::Disabled) {}

        bool ice;
        utils::Optional<bool> iceControlling;

        api::SrtpMode srtpMode;
    };

    struct Audio
    {
        std::string relayType;
        utils::Optional<Transport> transport;
    };

    struct Video
    {
        std::string relayType;
        utils::Optional<Transport> transport;
    };

    struct Data
    {
    };

    utils::Optional<Transport> bundleTransport;
    utils::Optional<Audio> audio;
    utils::Optional<Video> video;
    utils::Optional<Data> data;
    utils::Optional<uint32_t> idleTimeoutSeconds;
};

} // namespace api
