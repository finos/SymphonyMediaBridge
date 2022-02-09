#pragma once

#include "legacyapi/Conference.h"
#include <string>

namespace legacyapi
{

namespace Helpers
{

inline bool bundlingEnabled(const legacyapi::Conference& conference)
{
    if (!conference._channelBundles.empty())
    {
        return true;
    }

    for (auto& content : conference._contents)
    {
        for (auto& channel : content._channels)
        {
            if (channel._channelBundleId.isSet())
            {
                return true;
            }
        }
    }

    return false;
}

inline bool hasRecording(const nlohmann::json& jsonData)
{
    return jsonData.count("recording") > 0;
}

inline const std::string* getChannelBundleId(const legacyapi::Conference& conference)
{
    if (!conference._channelBundles.empty())
    {
        return &conference._channelBundles[0]._id;
    }

    if (conference._contents.empty())
    {
        return nullptr;
    }

    const auto& content = conference._contents[0];
    if (content._channels.empty())
    {
        return nullptr;
    }

    const auto& channel = content._channels[0];
    if (!channel._channelBundleId.isSet())
    {
        return nullptr;
    }

    return &channel._channelBundleId.get();
}

inline bool iceEnabled(const legacyapi::Transport& transport)
{
    if (transport._connection.isSet())
    {
        return false;
    }
    return !transport._xmlns.isSet() || transport._xmlns.get().compare("urn:xmpp:jingle:transports:ice-udp:1") == 0;
}

template <typename T>
const legacyapi::Transport* getTransport(const legacyapi::Conference& conference, const T& channelOrSctpConnection)
{
    if (channelOrSctpConnection._transport.isSet())
    {
        return &(channelOrSctpConnection._transport.get());
    }
    else
    {
        if (conference._channelBundles.empty() || !channelOrSctpConnection._channelBundleId.isSet())
        {
            return nullptr;
        }

        return &(conference._channelBundles[0]._transport);
    }
}

} // namespace Helpers

} // namespace legacyapi
