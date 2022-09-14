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
        Transport() : _ice(false), _dtls(false) {}

        bool _ice;
        utils::Optional<bool> _iceControlling;

        bool _dtls;
    };

    struct Audio
    {
        std::string _relayType;
        utils::Optional<Transport> _transport;
    };

    struct Video
    {
        std::string _relayType;
        utils::Optional<Transport> _transport;
    };

    struct Data
    {
    };

    utils::Optional<Transport> _bundleTransport;
    utils::Optional<Audio> _audio;
    utils::Optional<Video> _video;
    utils::Optional<Data> _data;
    utils::Optional<uint32_t> _idleTimeoutSeconds;
};

} // namespace api
