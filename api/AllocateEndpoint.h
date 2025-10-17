#pragma once

#include "api/RtcDescriptors.h"
#include "bridge/engine/SsrcRewrite.h"
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
        Transport() : ice(false), dtls(false), sdes(false), privatePort(false) {}

        bool ice;
        utils::Optional<bool> iceControlling;

        bool dtls;
        bool sdes;
        bool privatePort;
    };

    struct Audio
    {
        std::string relayType;
        utils::Optional<Transport> transport;

        bridge::MediaMode getMediaMode() const
        {
            if (0 == relayType.compare("mixed"))
            {
                return bridge::MediaMode::MIXED;
            }
            else if (relayType.compare("ssrc-rewrite") == 0)
            {
                return bridge::MediaMode::SSRC_REWRITE;
            }
            return bridge::MediaMode::FORWARD;
        }
    };

    struct Video
    {
        std::string relayType;
        utils::Optional<Transport> transport;

        bridge::MediaMode getMediaMode() const
        {
            if (relayType.compare("ssrc-rewrite") == 0)
            {
                return bridge::MediaMode::SSRC_REWRITE;
            }
            return bridge::MediaMode::FORWARD;
        }
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
