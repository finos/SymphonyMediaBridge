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
    setIfExists(transport.ice, data, "ice");
    setIfExists(transport.iceControlling, data, "ice-controlling");
    setIfExists(transport.dtls, data, "dtls");
    return transport;
}

api::AllocateEndpoint::Audio parseAllocateEndpointAudio(const nlohmann::json& data)
{
    api::AllocateEndpoint::Audio audio;
    setIfExists(audio.relayType, data, "relay-type");

    if (data.find("transport") != data.end())
    {
        audio.transport.set(parseAllocateEndpointTransport(data["transport"]));
    }

    return audio;
}

api::AllocateEndpoint::Video parseAllocateEndpointVideo(const nlohmann::json& data)
{
    api::AllocateEndpoint::Video video;
    setIfExists(video.relayType, data, "relay-type");

    if (data.find("transport") != data.end())
    {
        video.transport.set(parseAllocateEndpointTransport(data["transport"]));
    }

    return video;
}

api::Transport parsePatchEndpointTransport(const nlohmann::json& data)
{
    api::Transport transport;
    setIfExists(transport.rtcpMux, data, "rtcp-mux");

    if (data.find("ice") != data.end())
    {
        api::Ice ice = api::Parser::parseIce(data["ice"]);
        transport.ice.set(ice);
    }

    if (data.find("dtls") != data.end())
    {
        api::Dtls dtls;
        const auto& dtlsJson = data["dtls"];
        dtls.type = dtlsJson["type"].get<std::string>();
        dtls.hash = dtlsJson["hash"].get<std::string>();
        dtls.setup = dtlsJson["setup"].get<std::string>();
        transport.dtls.set(dtls);
    }

    if (data.find("connection") != data.end())
    {
        const auto& connectionJson = data["connection"];
        api::Connection connection;
        connection.port = connectionJson["port"].get<uint32_t>();
        connection.ip = connectionJson["ip"].get<std::string>();
        transport.connection.set(std::move(connection));
    }

    return transport;
}

api::PayloadType parsePatchEndpointPayloadType(const nlohmann::json& data)
{
    api::PayloadType payloadType;

    payloadType.id = data["id"].get<uint32_t>();
    payloadType.name = data["name"].get<std::string>();

    payloadType.clockRate = data["clockrate"].get<uint32_t>();

    setIfExists(payloadType.channels, data, "channels");

    if (data.find("parameters") != data.end())
    {
        const auto& parametersJson = data["parameters"];
        for (auto it = parametersJson.begin(); it != parametersJson.end(); ++it)
        {
            payloadType.parameters.emplace_back(std::make_pair(it.key(), it.value()));
        }
    }

    for (const auto& rtcpFbJson : optionalJsonArray(data, "rtcp-fbs"))
    {
        const auto& type = rtcpFbJson["type"].get<std::string>();
        if (rtcpFbJson.find("subtype") != rtcpFbJson.end())
        {
            const auto& subtype = rtcpFbJson["subtype"].get<std::string>();
            payloadType.rtcpFeedbacks.emplace_back(type, utils::Optional<std::string>(subtype));
        }
        else
        {
            payloadType.rtcpFeedbacks.emplace_back(type, utils::Optional<std::string>());
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
            allocateConference.lastN.set(lastN.get<uint32_t>());
        }
    }
    setIfExistsOrDefault(allocateConference.useGlobalPort, data, "global-port", true);

    return allocateConference;
}

AllocateEndpoint parseAllocateEndpoint(const nlohmann::json& data)
{
    AllocateEndpoint allocateEndpoint;

    if (data.find("bundle-transport") != data.end())
    {
        allocateEndpoint.bundleTransport.set(parseAllocateEndpointTransport(data["bundle-transport"]));
    }

    if (data.find("audio") != data.end())
    {
        allocateEndpoint.audio.set(parseAllocateEndpointAudio(data["audio"]));
    }

    if (data.find("video") != data.end())
    {
        allocateEndpoint.video.set(parseAllocateEndpointVideo(data["video"]));
    }

    if (data.find("data") != data.end())
    {
        allocateEndpoint.data.set(AllocateEndpoint::Data());
    }

    setIfExists(allocateEndpoint.idleTimeoutSeconds, data, "idleTimeout");
    return allocateEndpoint;
}

EndpointDescription parsePatchEndpoint(const nlohmann::json& data, const std::string& endpointId)
{
    EndpointDescription endpointDescription;
    endpointDescription.endpointId = endpointId;

    if (data.find("bundle-transport") != data.end())
    {
        endpointDescription.bundleTransport.set(parsePatchEndpointTransport(data["bundle-transport"]));
    }

    if (data.find("audio") != data.end())
    {
        api::Audio audioChannel;
        const auto& audioJson = data["audio"];

        if (audioJson.find("transport") != audioJson.end())
        {
            audioChannel.transport.set(parsePatchEndpointTransport(audioJson["transport"]));
        }

        for (const auto& ssrcJson : optionalJsonArray(audioJson, "ssrcs"))
        {
            const auto ssrc = ssrcJson.get<uint32_t>();
            audioChannel.ssrcs.push_back(ssrc);
        }

        if (audioJson.find("payload-type") != audioJson.end())
        {
            audioChannel.payloadType.set(parsePatchEndpointPayloadType(audioJson["payload-type"]));
        }

        for (const auto& rtpHdrExtJson : optionalJsonArray(audioJson, "rtp-hdrexts"))
        {
            const auto id = rtpHdrExtJson["id"].get<uint32_t>();
            if (id > 0 && id < 15)
            {
                audioChannel.rtpHeaderExtensions.emplace_back(id, rtpHdrExtJson["uri"].get<std::string>());
            }
        }

        endpointDescription.audio.set(std::move(audioChannel));
    }

    if (data.find("video") != data.end())
    {
        api::Video videoChannel;
        const auto& videoJson = data["video"];

        if (videoJson.find("transport") != videoJson.end())
        {
            videoChannel.transport.set(parsePatchEndpointTransport(videoJson["transport"]));
        }

        for (const auto& payloadTypeJson : optionalJsonArray(videoJson, "payload-types"))
        {
            videoChannel.payloadTypes.emplace_back(parsePatchEndpointPayloadType(payloadTypeJson));
        }

        for (const auto& rtpHdrExtJson : optionalJsonArray(videoJson, "rtp-hdrexts"))
        {
            const auto id = rtpHdrExtJson["id"].get<uint32_t>();
            if (id > 0 && id < 15)
            {
                videoChannel.rtpHeaderExtensions.emplace_back(id, rtpHdrExtJson["uri"].get<std::string>());
            }
        }

        for (const auto& stream : optionalJsonArray(videoJson, "streams"))
        {
            videoChannel.streams.emplace_back();
            auto& videoStream = videoChannel.streams.back();
            for (const auto& rtpSource : requiredJsonArray(stream, "sources"))
            {
                api::SsrcPair level;
                level.main = rtpSource["main"].get<uint32_t>();
                setIfExists(level.feedback, rtpSource, "feedback");
                videoStream.sources.push_back(level);
            }
            videoStream.content = stream["content"];
        }

        if (videoJson.find("ssrc-whitelist") != videoJson.end())
        {
            std::vector<uint32_t> ssrcWhitelist;
            for (const auto& ssrcJson : videoJson["ssrc-whitelist"])
            {
                const auto ssrc = ssrcJson.get<uint32_t>();
                ssrcWhitelist.push_back(ssrc);
            }
            videoChannel.ssrcWhitelist.set(std::move(ssrcWhitelist));
        }

        endpointDescription.video.set(std::move(videoChannel));
    }

    if (data.find("data") != data.end())
    {
        api::Data dataChannel;
        const auto& dataJson = data["data"];
        const auto& portJson = dataJson["port"];
        dataChannel.port = portJson.get<uint32_t>();
        endpointDescription.data.set(dataChannel);
    }

    if (data.find("neighbours") != data.end())
    {
        for (auto& group : data["neighbours"]["groups"])
        {
            endpointDescription.neighbours.push_back(group);
        }
    }
    return endpointDescription;
}

Recording parseRecording(const nlohmann::json& data)
{
    Recording recording;

    const auto& recordingJson = data["recording"];

    recording.recordingId = recordingJson["recording-id"].get<std::string>();
    recording.userId = recordingJson["user-id"].get<std::string>();

    const auto& modalities = recordingJson["recording-modalities"];
    setIfExistsOrDefault<>(recording.isAudioEnabled, modalities, "audio", false);
    setIfExistsOrDefault<>(recording.isVideoEnabled, modalities, "video", false);
    setIfExistsOrDefault<>(recording.isScreenshareEnabled, modalities, "screenshare", false);

    for (const auto& channelJson : optionalJsonArray(recordingJson, "channels"))
    {
        api::RecordingChannel recordingChannel;
        setIfExists<>(recordingChannel.id, channelJson, "id");
        setIfExists<>(recordingChannel.host, channelJson, "host");
        setIfExistsOrDefault<>(recordingChannel.port, channelJson, "port", uint16_t(0));

        std::string aesKeyEnc;
        std::string saltEnc;
        setIfExists<>(aesKeyEnc, channelJson, "aes-key");
        setIfExists<>(saltEnc, channelJson, "aes-salt");

        if (!aesKeyEnc.empty())
        {
            ::utils::Base64::decode(aesKeyEnc, recordingChannel.aesKey, 32);
        }

        if (!saltEnc.empty())
        {
            ::utils::Base64::decode(saltEnc, recordingChannel.aesSalt, 12);
        }

        recording.channels.emplace_back(recordingChannel);
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
            // do nothing
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
            const auto& userId = inner.key();
            assert(inner.value().is_object());

            setIfExistsOrThrow(endpoint.ssrcOriginal, inner.value(), "ssrcOriginal");
            setIfExistsOrThrow(endpoint.ssrcRewritten, inner.value(), "ssrcRewritten");
            endpoint.userId.set(std::stoul(userId));
            break;
        }
    }
    return endpoint;
}

Ice parseIce(const nlohmann::json& iceJson)
{
    api::Ice ice;
    ice.ufrag = iceJson["ufrag"].get<std::string>();
    ice.pwd = iceJson["pwd"].get<std::string>();

    for (const auto& candidateJson : optionalJsonArray(iceJson, "candidates"))
    {
        api::Candidate candidate;
        candidate.generation = candidateJson["generation"].get<uint32_t>();
        candidate.component = candidateJson["component"].get<uint32_t>();
        candidate.protocol = candidateJson["protocol"].get<std::string>();
        candidate.port = candidateJson["port"].get<uint32_t>();
        candidate.ip = candidateJson["ip"].get<std::string>();
        setIfExists(candidate.relPort, candidateJson, "rel-port");
        setIfExists(candidate.relAddr, candidateJson, "rel-addr");
        candidate.foundation = candidateJson["foundation"].get<std::string>();
        candidate.priority = candidateJson["priority"].get<uint32_t>();
        candidate.type = candidateJson["type"].get<std::string>();
        candidate.network =
            candidateJson.find("network") != candidateJson.end() ? candidateJson["network"].get<uint32_t>() : 0;
        ice.candidates.emplace_back(std::move(candidate));
    }

    return ice;
}

BarbellDescription parsePatchBarbell(const nlohmann::json& data, const std::string& barbellId)
{
    BarbellDescription barbellDescription;
    barbellDescription.barbellId = barbellId;

    if (data.find("bundle-transport") != data.end())
    {
        barbellDescription.transport = parsePatchEndpointTransport(data["bundle-transport"]);
    }

    if (data.find("audio") != data.end())
    {
        api::Audio audioChannel;
        const auto& audioJson = data["audio"];

        for (const auto& ssrcJson : optionalJsonArray(audioJson, "ssrcs"))
        {
            const auto ssrc = ssrcJson.get<uint32_t>();
            audioChannel.ssrcs.push_back(ssrc);
        }

        if (audioJson.find("payload-type") != audioJson.end())
        {
            audioChannel.payloadType.set(parsePatchEndpointPayloadType(audioJson["payload-type"]));
        }

        for (const auto& rtpHdrExtJson : optionalJsonArray(audioJson, "rtp-hdrexts"))
        {
            const auto id = rtpHdrExtJson["id"].get<uint32_t>();
            if (id > 0 && id < 15)
            {
                audioChannel.rtpHeaderExtensions.emplace_back(id, rtpHdrExtJson["uri"].get<std::string>());
            }
        }

        barbellDescription.audio = audioChannel;
    }

    if (data.find("video") != data.end())
    {
        api::Video videoChannel;
        const auto& videoJson = data["video"];

        for (const auto& payloadTypeJson : optionalJsonArray(videoJson, "payload-types"))
        {
            videoChannel.payloadTypes.emplace_back(parsePatchEndpointPayloadType(payloadTypeJson));
        }

        for (const auto& rtpHdrExtJson : optionalJsonArray(videoJson, "rtp-hdrexts"))
        {
            const auto id = rtpHdrExtJson["id"].get<uint32_t>();
            if (id > 0 && id < 15)
            {
                videoChannel.rtpHeaderExtensions.emplace_back(id, rtpHdrExtJson["uri"].get<std::string>());
            }
        }

        for (const auto& stream : optionalJsonArray(videoJson, "streams"))
        {
            videoChannel.streams.emplace_back();
            auto& videoStream = videoChannel.streams.back();
            for (const auto& rtpSource : requiredJsonArray(stream, "sources"))
            {
                api::SsrcPair level;
                level.main = rtpSource["main"].get<uint32_t>();
                setIfExists(level.feedback, rtpSource, "feedback");
                videoStream.sources.push_back(level);
            }
            videoStream.content = stream["content"];
        }

        barbellDescription.video = videoChannel;
    }

    if (data.find("data") != data.end())
    {
        api::Data dataChannel;
        const auto& dataJson = data["data"];
        const auto& portJson = dataJson["port"];
        dataChannel.port = portJson.get<uint32_t>();
        barbellDescription.data = dataChannel;
    }

    return barbellDescription;
}
} // namespace Parser

} // namespace api
