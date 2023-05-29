#pragma once

#include "bridge/engine/SsrcRewrite.h"
#include "legacyapi/PayloadType.h"
#include "legacyapi/SsrcAttribute.h"
#include "legacyapi/SsrcGroup.h"
#include "legacyapi/Transport.h"
#include "utils/Optional.h"
#include <cstdint>
#include <string>
#include <vector>

namespace legacyapi
{

struct Channel
{
    struct RtpHdrExt
    {
        uint32_t _id;
        std::string _uri;
    };

    utils::Optional<std::string> _id;
    utils::Optional<std::string> _endpoint;
    utils::Optional<std::string> _channelBundleId;
    std::vector<uint32_t> _sources;
    std::vector<SsrcGroup> _ssrcGroups;
    utils::Optional<bool> _initiator;
    utils::Optional<std::string> _rtpLevelRelayType;
    utils::Optional<uint32_t> _expire;
    utils::Optional<std::string> _direction;
    utils::Optional<Transport> _transport;
    utils::Optional<int32_t> _lastN;
    std::vector<PayloadType> _payloadTypes;
    std::vector<RtpHdrExt> _rtpHeaderHdrExts;
    utils::Optional<std::vector<uint32_t>> _ssrcWhitelist;
    std::vector<SsrcAttribute> _ssrcAttributes;

    bool isRelayTypeMixer() const
    {
        return _rtpLevelRelayType.isSet() && _rtpLevelRelayType.get().compare("mixer") == 0;
    }

    bool isRelayTypeRewrite() const
    {
        return _rtpLevelRelayType.isSet() && _rtpLevelRelayType.get().compare("ssrc-rewrite") == 0;
    }

    bridge::MediaMode getMediaMode() const
    {
        if (_rtpLevelRelayType.isSet())
        {
            if (0 == _rtpLevelRelayType.get().compare("mixer"))
            {
                return bridge::MediaMode::MIXED;
            }
            else if (_rtpLevelRelayType.get().compare("ssrc-rewrite") == 0)
            {
                return bridge::MediaMode::SSRC_REWRITE;
            }
        }
        return bridge::MediaMode::FORWARD;
    }
};

} // namespace legacyapi
