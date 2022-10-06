#pragma once

#include "utils/Optional.h"
#include <cstdint>
#include <string>

namespace api
{

struct AllocateEndpoint
{
    struct Transport
    {
        Transport() : ice(false), dtls(false) {}

        bool ice;
        utils::Optional<bool> iceControlling;

        bool dtls;
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
