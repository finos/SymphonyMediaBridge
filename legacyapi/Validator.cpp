#include "legacyapi/Validator.h"
#include "legacyapi/Conference.h"
#include "legacyapi/Content.h"
#include "legacyapi/PatchConferenceType.h"

namespace
{

bool validateAVContent(const legacyapi::Conference& conference,
    const legacyapi::Content& content,
    legacyapi::PatchConferenceType& patchConferenceType)
{
    if (content._channels.empty())
    {
        return false;
    }

    if (!content._sctpConnections.empty())
    {
        return false;
    }

    for (const auto& channel : content._channels)
    {
        if (!channel._id.isSet())
        {
            // Allocate channel

            if (patchConferenceType != legacyapi::PatchConferenceType::Undefined &&
                patchConferenceType != legacyapi::PatchConferenceType::AllocateChannels)
            {
                return false;
            }
            patchConferenceType = legacyapi::PatchConferenceType::AllocateChannels;

            if (!channel._initiator.isSet() || !channel._endpoint.isSet() || !channel._direction.isSet())
            {
                return false;
            }
        }
        else
        {
            // Configure, reconfigure or expire channel

            if (patchConferenceType == legacyapi::PatchConferenceType::AllocateChannels)
            {
                return false;
            }

            if (channel._expire.isSet() && channel._expire.get() == 0)
            {
                // Expire channel

                if (patchConferenceType != legacyapi::PatchConferenceType::Undefined &&
                    patchConferenceType != legacyapi::PatchConferenceType::ExpireChannels)
                {
                    return false;
                }
                patchConferenceType = legacyapi::PatchConferenceType::ExpireChannels;
            }
            else
            {
                if (patchConferenceType == legacyapi::PatchConferenceType::AllocateChannels ||
                    patchConferenceType == legacyapi::PatchConferenceType::ExpireChannels)
                {
                    return false;
                }
                patchConferenceType = legacyapi::PatchConferenceType::ConfigureChannels;

                // Configure or reconfigure
                if (!channel._channelBundleId.isSet() && !channel._transport.isSet())
                {
                    return false;
                }

                if (!channel._endpoint.isSet() || !channel._direction.isSet())
                {
                    return false;
                }

                if (channel._ssrcWhitelist.isSet() && channel._ssrcWhitelist.get().size() > 2)
                {
                    return false;
                }
            }
        }
    }

    return true;
}

bool validateDataContent(const legacyapi::Content& content, legacyapi::PatchConferenceType& patchConferenceType)
{
    if (content._sctpConnections.empty())
    {
        return false;
    }

    if (!content._channels.empty())
    {
        return false;
    }

    return true;
}

bool isRecordingStart(const api::Recording& recording)
{
    return recording._isAudioEnabled || recording._isVideoEnabled || recording._isScreenshareEnabled;
}

bool recordingChannelsAreValid(const api::Recording& recording)
{
    for (const auto& channel : recording._channels)
    {
        if (!(channel._port > 0 && !channel._host.empty()))
        {
            return false;
        }
    }
    return true;
}

} // namespace

namespace legacyapi
{

namespace Validator
{

bool isValid(const Conference& conference)
{
    if (conference._id.empty())
    {
        return false;
    }

    if (conference._contents.empty() && !conference._recording.isSet())
    {
        return false;
    }

    // Check if recording channel is valid when a recording start is requested
    if (conference._recording.isSet() && isRecordingStart(conference._recording.get()) &&
        !recordingChannelsAreValid(conference._recording.get()))
    {
        return false;
    }

    auto patchConferenceType = legacyapi::PatchConferenceType::Undefined;

    for (const auto& content : conference._contents)
    {
        if (content._name.empty())
        {
            return false;
        }

        if (content._name.compare("audio") == 0)
        {
            if (!validateAVContent(conference, content, patchConferenceType))
            {
                return false;
            }
        }
        else if (content._name.compare("video") == 0)
        {
            if (!validateAVContent(conference, content, patchConferenceType))
            {
                return false;
            }
        }
        else if (content._name.compare("data") == 0)
        {
            if (!validateDataContent(content, patchConferenceType))
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }

    return true;
}

} // namespace Validator

} // namespace legacyapi
