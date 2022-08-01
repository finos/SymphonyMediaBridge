#include "api/Parser.h"
#include "api/ConferenceEndpoint.h"
#include "api/utils.h"
#include "utils/Base64.h"

namespace
{

const nlohmann::json& optionalJsonArray(const nlohmann::json& data, const char* arrayProperty)
{
    static const nlohmann::json EMPTY_JSON_ARRAY = nlohmann::json::array();

    const auto it = data.find(arrayProperty);
    return it == data.end() ? EMPTY_JSON_ARRAY : *it;
}

const nlohmann::json& requiredJsonArray(const nlohmann::json& data, const char* arrayProperty)
{
    const auto it = data.find(arrayProperty);
    if (it == data.end())
    {
        const auto sb = std::string().append("Missing required array property: ").append(arrayProperty);

        throw nlohmann::detail::other_error::create(-1, sb);
    }

    return *it;
}

template <typename T>
void setIfExistsOrThrow(T& target, const nlohmann::json& data, const char* name)
{
    const auto& it = data.find(name);
    if (it != data.end())
    {
        target = it->get<T>();
    }
    else
    {
        const auto sb = std::string().append("Missing required property: ").append(name);
        throw nlohmann::detail::other_error::create(-1, sb);
    }
}

template <typename T>
void setIfExists(utils::Optional<T>& target, const nlohmann::json& data, const char* name)
{
    if (data.find(name) != data.end())
    {
        target.set(data[name].get<typename utils::Optional<T>::ValueType>());
    }
}

template <typename T>
void setIfExists(T& target, const nlohmann::json& data, const char* name)
{
    const auto& it = data.find(name);
    if (it != data.end())
    {
        target = it->get<T>();
    }
}

template <typename T>
void setIfExistsOrDefault(T& target, const nlohmann::json& data, const char* name, T&& defaultValue)
{
    const auto& it = data.find(name);
    if (it != data.end())
    {
        target = it->get<T>();
    }
    else
    {
        target = std::forward<T>(defaultValue);
    }
}

template <>
void setIfExists(utils::Optional<bool>& target, const nlohmann::json& data, const char* name)
{
    if (data.find(name) != data.end())
    {
        const auto& jsonElement = data[name];
        if (jsonElement.is_string())
        {
            target.set(data[name].get<std::string>().compare("true") == 0);
        }
        else
        {
            target.set(data[name].get<bool>());
        }
    }
}

template <>
void setIfExists(utils::Optional<std::string>& target, const nlohmann::json& data, const char* name)
{
    if (data.find(name) != data.end())
    {
        const auto& jsonElement = data[name];
        if (jsonElement.is_string())
        {
            target.set(data[name].get<std::string>());
        }
        else if (jsonElement.is_number_integer())
        {
            target.set(std::to_string(data[name].get<int32_t>()));
        }
        else if (jsonElement.is_boolean())
        {
            if (jsonElement.get<bool>())
            {
                target.set("true");
            }
            else
            {
                target.set("false");
            }
        }
    }
}

api::AllocateEndpoint::Transport parseAllocateEndpointTransport(const nlohmann::json& data)
{
    api::AllocateEndpoint::Transport transport;
    setIfExists(transport._ice, data, "ice");
    setIfExists(transport._iceControlling, data, "ice-controlling");
    setIfExists(transport._dtls, data, "dtls");
    return transport;
}

api::AllocateEndpoint::Audio parseAllocateEndpointAudio(const nlohmann::json& data)
{
    api::AllocateEndpoint::Audio audio;
    setIfExists(audio._relayType, data, "relay-type");

    if (data.find("transport") != data.end())
    {
        audio._transport.set(parseAllocateEndpointTransport(data["transport"]));
    }

    return audio;
}

api::AllocateEndpoint::Video parseAllocateEndpointVideo(const nlohmann::json& data)
{
    api::AllocateEndpoint::Video video;
    setIfExists(video._relayType, data, "relay-type");

    if (data.find("transport") != data.end())
    {
        video._transport.set(parseAllocateEndpointTransport(data["transport"]));
    }

    return video;
}

api::EndpointDescription::Transport parsePatchEndpointTransport(const nlohmann::json& data)
{
    api::EndpointDescription::Transport transport;
    setIfExists(transport._rtcpMux, data, "rtcp-mux");

    if (data.find("ice") != data.end())
    {
        const auto& iceJson = data["ice"];
        api::EndpointDescription::Ice ice;
        ice._ufrag = iceJson["ufrag"].get<std::string>();
        ice._pwd = iceJson["pwd"].get<std::string>();

        for (const auto& candidateJson : iceJson["candidates"])
        {
            api::EndpointDescription::Candidate candidate;
            candidate._generation = candidateJson["generation"].get<uint32_t>();
            candidate._component = candidateJson["component"].get<uint32_t>();
            candidate._protocol = candidateJson["protocol"].get<std::string>();
            candidate._port = candidateJson["port"].get<uint32_t>();
            candidate._ip = candidateJson["ip"].get<std::string>();
            setIfExists(candidate._relPort, candidateJson, "rel-port");
            setIfExists(candidate._relAddr, candidateJson, "rel-addr");
            candidate._foundation = candidateJson["foundation"].get<std::string>();
            candidate._priority = candidateJson["priority"].get<uint32_t>();
            candidate._type = candidateJson["type"].get<std::string>();
            candidate._network =
                candidateJson.find("network") != candidateJson.end() ? candidateJson["network"].get<uint32_t>() : 0;
            ice._candidates.emplace_back(std::move(candidate));
        }

        transport._ice.set(ice);
    }

    if (data.find("dtls") != data.end())
    {
        api::EndpointDescription::Dtls dtls;
        const auto& dtlsJson = data["dtls"];
        dtls.type = dtlsJson["type"].get<std::string>();
        dtls.hash = dtlsJson["hash"].get<std::string>();
        dtls.setup = dtlsJson["setup"].get<std::string>();
        transport._dtls.set(dtls);
    }

    if (data.find("connection") != data.end())
    {
        const auto& connectionJson = data["connection"];
        api::EndpointDescription::Connection connection;
        connection._port = connectionJson["port"].get<uint32_t>();
        connection._ip = connectionJson["ip"].get<std::string>();
        transport._connection.set(std::move(connection));
    }

    return transport;
}

api::EndpointDescription::PayloadType parsePatchEndpointPayloadType(const nlohmann::json& data)
{
    api::EndpointDescription::PayloadType payloadType;

    payloadType._id = data["id"].get<uint32_t>();
    payloadType._name = data["name"].get<std::string>();

    {
        const auto& clockRate = data["clockrate"];
        if (clockRate.is_string())
        {
            payloadType._clockRate = std::stoul(data["clockrate"].get<std::string>());
        }
        else
        {
            payloadType._clockRate = data["clockrate"].get<uint32_t>();
        }
    }

    setIfExists(payloadType._channels, data, "channels");

    if (data.find("parameters") != data.end())
    {
        const auto& parametersJson = data["parameters"];
        for (auto it = parametersJson.begin(); it != parametersJson.end(); ++it)
        {
            payloadType._parameters.emplace_back(std::make_pair(it.key(), it.value()));
        }
    }

    for (const auto& rtcpFbJson : optionalJsonArray(data, "rtcp-fbs"))
    {
        const auto& type = rtcpFbJson["type"].get<std::string>();
        if (rtcpFbJson.find("subtype") != rtcpFbJson.end())
        {
            const auto& subtype = rtcpFbJson["subtype"].get<std::string>();
            payloadType._rtcpFeedbacks.emplace_back(type, utils::Optional<std::string>(subtype));
        }
        else
        {
            payloadType._rtcpFeedbacks.emplace_back(type, utils::Optional<std::string>());
        }
    }

    return payloadType;
}

} // namespace

namespace api
{

namespace Parser
{

AllocateConference parseAllocateConference(const nlohmann::json& data)
{
    AllocateConference allocateConference;
    if (data.find("last-n") != data.end())
    {
        const auto& lastN = data["last-n"];
        if (lastN.is_number_integer())
        {
            allocateConference._lastN.set(lastN.get<uint32_t>());
        }
    }

    return allocateConference;
}

AllocateEndpoint parseAllocateEndpoint(const nlohmann::json& data)
{
    AllocateEndpoint allocateEndpoint;

    if (data.find("bundle-transport") != data.end())
    {
        allocateEndpoint._bundleTransport.set(parseAllocateEndpointTransport(data["bundle-transport"]));
    }

    if (data.find("audio") != data.end())
    {
        allocateEndpoint._audio.set(parseAllocateEndpointAudio(data["audio"]));
    }

    if (data.find("video") != data.end())
    {
        allocateEndpoint._video.set(parseAllocateEndpointVideo(data["video"]));
    }

    if (data.find("data") != data.end())
    {
        allocateEndpoint._data.set(AllocateEndpoint::Data());
    }

    return allocateEndpoint;
}

EndpointDescription parsePatchEndpoint(const nlohmann::json& data, const std::string& endpointId)
{
    EndpointDescription endpointDescription;
    endpointDescription._endpointId = endpointId;

    if (data.find("bundle-transport") != data.end())
    {
        endpointDescription._bundleTransport.set(parsePatchEndpointTransport(data["bundle-transport"]));
    }

    if (data.find("audio") != data.end())
    {
        api::EndpointDescription::Audio audioChannel;
        const auto& audioJson = data["audio"];

        if (audioJson.find("transport") != audioJson.end())
        {
            audioChannel._transport.set(parsePatchEndpointTransport(audioJson["transport"]));
        }

        for (const auto& ssrcJson : optionalJsonArray(audioJson, "ssrcs"))
        {
            const auto ssrc = ssrcJson.is_string() ? std::stoul(ssrcJson.get<std::string>()) : ssrcJson.get<uint32_t>();
            audioChannel._ssrcs.push_back(ssrc);
        }

        if (audioJson.find("payload-type") != audioJson.end())
        {
            audioChannel._payloadType.set(parsePatchEndpointPayloadType(audioJson["payload-type"]));
        }

        for (const auto& rtpHdrExtJson : optionalJsonArray(audioJson, "rtp-hdrexts"))
        {
            const auto id = rtpHdrExtJson["id"].get<uint32_t>();
            if (id > 0 && id < 15)
            {
                audioChannel._rtpHeaderExtensions.emplace_back(id, rtpHdrExtJson["uri"].get<std::string>());
            }
        }

        endpointDescription._audio.set(std::move(audioChannel));
    }

    if (data.find("video") != data.end())
    {
        api::EndpointDescription::Video videoChannel;
        const auto& videoJson = data["video"];

        if (videoJson.find("transport") != videoJson.end())
        {
            videoChannel._transport.set(parsePatchEndpointTransport(videoJson["transport"]));
        }

        for (const auto& ssrcJson : optionalJsonArray(videoJson, "ssrcs"))
        {
            const auto ssrc = ssrcJson.is_string() ? std::stoul(ssrcJson.get<std::string>()) : ssrcJson.get<uint32_t>();
            videoChannel._ssrcs.push_back(ssrc);
        }

        for (const auto& payloadTypeJson : optionalJsonArray(videoJson, "payload-types"))
        {
            videoChannel._payloadTypes.emplace_back(parsePatchEndpointPayloadType(payloadTypeJson));
        }

        for (const auto& rtpHdrExtJson : optionalJsonArray(videoJson, "rtp-hdrexts"))
        {
            const auto id = rtpHdrExtJson["id"].get<uint32_t>();
            if (id > 0 && id < 15)
            {
                videoChannel._rtpHeaderExtensions.emplace_back(id, rtpHdrExtJson["uri"].get<std::string>());
            }
        }

        for (const auto& ssrcGroupJson : optionalJsonArray(videoJson, "ssrc-groups"))
        {
            api::EndpointDescription::SsrcGroup ssrcGroup;
            for (const auto& ssrcJson : requiredJsonArray(ssrcGroupJson, "ssrcs"))
            {
                const uint32_t ssrc =
                    ssrcJson.is_string() ? std::stoul(ssrcJson.get<std::string>()) : ssrcJson.get<uint32_t>();
                ssrcGroup._ssrcs.push_back(ssrc);
            }
            ssrcGroup._semantics = ssrcGroupJson["semantics"].get<std::string>();
            videoChannel._ssrcGroups.emplace_back(std::move(ssrcGroup));
        }

        for (const auto& ssrcAttributeJson : optionalJsonArray(videoJson, "ssrc-attributes"))
        {
            api::EndpointDescription::SsrcAttribute ssrcAttribute;
            ssrcAttribute._content = ssrcAttributeJson["content"].get<std::string>();
            for (const auto& ssrcJson : requiredJsonArray(ssrcAttributeJson, "ssrcs"))
            {
                const auto ssrc =
                    ssrcJson.is_string() ? std::stoul(ssrcJson.get<std::string>()) : ssrcJson.get<uint32_t>();
                ssrcAttribute._ssrcs.push_back(ssrc);
            }
            videoChannel._ssrcAttributes.push_back(ssrcAttribute);
        }

        if (videoJson.find("ssrc-whitelist") != videoJson.end())
        {
            std::vector<uint32_t> ssrcWhitelist;
            for (const auto& ssrcJson : videoJson["ssrc-whitelist"])
            {
                const auto ssrc =
                    ssrcJson.is_string() ? std::stoul(ssrcJson.get<std::string>()) : ssrcJson.get<uint32_t>();
                ssrcWhitelist.push_back(ssrc);
            }
            videoChannel._ssrcWhitelist.set(std::move(ssrcWhitelist));
        }

        endpointDescription._video.set(std::move(videoChannel));
    }

    if (data.find("data") != data.end())
    {
        api::EndpointDescription::Data dataChannel;
        const auto& dataJson = data["data"];
        const auto& portJson = dataJson["port"];
        dataChannel._port = portJson.is_string() ? std::stoul(portJson.get<std::string>()) : portJson.get<uint32_t>();
        endpointDescription._data.set(dataChannel);
    }

    return endpointDescription;
}

Recording parseRecording(const nlohmann::json& data)
{
    Recording recording;

    const auto& recordingJson = data["recording"];

    recording._recordingId = recordingJson["recording-id"].get<std::string>();
    recording._userId = recordingJson["user-id"].get<std::string>();

    const auto& modalities = recordingJson["recording-modalities"];
    setIfExistsOrDefault<>(recording._isAudioEnabled, modalities, "audio", false);
    setIfExistsOrDefault<>(recording._isVideoEnabled, modalities, "video", false);
    setIfExistsOrDefault<>(recording._isScreenshareEnabled, modalities, "screenshare", false);

    for (const auto& channelJson : optionalJsonArray(recordingJson, "channels"))
    {
        api::RecordingChannel recordingChannel;
        setIfExists<>(recordingChannel._id, channelJson, "id");
        setIfExists<>(recordingChannel._host, channelJson, "host");
        setIfExistsOrDefault<>(recordingChannel._port, channelJson, "port", uint16_t(0));

        std::string aesKeyEnc;
        std::string saltEnc;
        setIfExists<>(aesKeyEnc, channelJson, "aes-key");
        setIfExists<>(saltEnc, channelJson, "aes-salt");

        if (!aesKeyEnc.empty())
        {
            ::utils::Base64::decode(aesKeyEnc, recordingChannel._aesKey, 32);
        }

        if (!saltEnc.empty())
        {
            ::utils::Base64::decode(saltEnc, recordingChannel._aesSalt, 12);
        }

        recording._channels.emplace_back(recordingChannel);
    }

    return recording;
}

ConferenceEndpoint parseConferenceEndpoint(const nlohmann::json& data)
{
    ConferenceEndpoint endpoint;
    setIfExistsOrThrow<>(endpoint.id, data, "id");
    setIfExistsOrThrow<>(endpoint.isDominantSpeaker, data, "isDominantSpeaker");
    setIfExistsOrThrow<>(endpoint.isActiveTalker, data, "isActiveTalker");
    std::string iceStateStr;
    std::string dtlsStateStr;
    setIfExistsOrThrow<>(iceStateStr, data, "iceState");
    setIfExistsOrThrow<>(dtlsStateStr, data, "dtlsState");

    ice::IceSession::State iceState = utils::stringToIceState(iceStateStr);
    if (iceState != ice::IceSession::State::LAST)
    {
        endpoint.iceState = iceState;
    }
    transport::SrtpClient::State dtlsState = utils::stringToDtlsState(dtlsStateStr);
    if (dtlsState != transport::SrtpClient::State::LAST)
    {
        endpoint.dtlsState = dtlsState;
    }
    return endpoint;
}

std::vector<ConferenceEndpoint> parseConferenceEndpoints(const nlohmann::json& responseBody)
{
    std::vector<ConferenceEndpoint> endpoints;
    assert(responseBody.is_array());
    for (const auto& elem : responseBody)
    {
        try
        {
            endpoints.push_back(Parser::parseConferenceEndpoint(elem));
        }
        catch (...)
        {
            // do nothng
        }
    }
    return endpoints;
}

ConferenceEndpointExtendedInfo parseEndpointExtendedInfo(const nlohmann::json& data)
{
    ConferenceEndpointExtendedInfo endpoint;
    endpoint.basicEndpointInfo = parseConferenceEndpoint(data);
    nlohmann::json iceSelectedTuple = nlohmann::json::object();

    setIfExistsOrThrow<>(iceSelectedTuple, data, "iceSelectedTuple");
    setIfExistsOrThrow<>(endpoint.localIP, iceSelectedTuple, "localIP");
    setIfExistsOrThrow<>(endpoint.remoteIP, iceSelectedTuple, "remoteIP");
    setIfExistsOrThrow<>(endpoint.localPort, iceSelectedTuple, "localPort");
    setIfExistsOrThrow<>(endpoint.remotePort, iceSelectedTuple, "remotePort");
    setIfExistsOrThrow<>(endpoint.protocol, iceSelectedTuple, "protocol");

    for (const auto& it : requiredJsonArray(data, "audioUserIdToSsrcMap"))
    {
        for (const auto& inner : it.items())
        {
            const auto& usid = inner.key();
            assert(inner.value().is_string());
            endpoint.usid = std::stoul(usid);
            endpoint.ssrc = std::stoul(inner.value().get<std::string>());
        }
    }
    return endpoint;
}

} // namespace Parser

} // namespace api