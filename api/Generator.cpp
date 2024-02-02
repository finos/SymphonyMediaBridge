#include "api/Generator.h"
#include "api/BarbellDescription.h"
#include "api/ConferenceEndpoint.h"
#include "api/EndpointDescription.h"
#include "api/utils.h"
#include "utils/Base64.h"

namespace
{

template <typename T>
void setIfExists(nlohmann::json& target, const char* name, const T& value)
{
    if (value.isSet())
    {
        target[name] = value.get();
    }
}

nlohmann::json generateTransport(const api::Transport& transport)
{
    nlohmann::json transportJson;
    transportJson["rtcp-mux"] = transport.rtcpMux;

    if (transport.ice.isSet())
    {
        const auto& ice = transport.ice.get();
        nlohmann::json iceJson;
        iceJson["ufrag"] = ice.ufrag;
        iceJson["pwd"] = ice.pwd;

        iceJson["candidates"] = nlohmann::json::array();
        for (const auto& candidate : ice.candidates)
        {
            nlohmann::json candidateJson;
            candidateJson["generation"] = candidate.generation;
            candidateJson["component"] = candidate.component;
            candidateJson["protocol"] = candidate.protocol;
            candidateJson["port"] = candidate.port;
            candidateJson["ip"] = candidate.ip;
            candidateJson["foundation"] = candidate.foundation;
            candidateJson["priority"] = candidate.priority;
            candidateJson["type"] = candidate.type;
            candidateJson["network"] = candidate.network;
            setIfExists(candidateJson, "rel-port", candidate.relPort);
            setIfExists(candidateJson, "rel-addr", candidate.relAddr);
            iceJson["candidates"].push_back(candidateJson);
        }

        transportJson["ice"] = iceJson;
    }

    if (transport.dtls.isSet())
    {
        const auto& dtls = transport.dtls.get();
        nlohmann::json dtlsJson;
        dtlsJson["type"] = dtls.type;
        dtlsJson["hash"] = dtls.hash;
        dtlsJson["setup"] = dtls.setup;
        transportJson["dtls"] = dtlsJson;
    }
    if (!transport.sdesKeys.empty())
    {
        transportJson["sdes"] = nlohmann::json::array();
        for (auto sdesKey : transport.sdesKeys)
        {
            nlohmann::json sdesJson;
            sdesJson["key"] = utils::Base64::encode(sdesKey.keySalt, sdesKey.getLength());
            sdesJson["profile"] = api::utils::toString(sdesKey.profile);
            transportJson["sdes"].push_back(sdesJson);
        }
    }

    if (transport.connection.isSet())
    {
        nlohmann::json connectionJson;
        connectionJson["port"] = transport.connection.get().port;
        connectionJson["ip"] = transport.connection.get().ip;
        transportJson["connection"] = connectionJson;
    }

    return transportJson;
}

nlohmann::json generatePayloadType(const api::PayloadType& payloadType)
{
    nlohmann::json payloadTypeJson;

    payloadTypeJson["id"] = payloadType.id;
    payloadTypeJson["name"] = payloadType.name;
    payloadTypeJson["clockrate"] = payloadType.clockRate;
    setIfExists(payloadTypeJson, "channels", payloadType.channels);

    payloadTypeJson["parameters"] = nlohmann::json::object();

    for (const auto& parameter : payloadType.parameters)
    {
        payloadTypeJson["parameters"][parameter.first] = parameter.second;
    }

    payloadTypeJson["rtcp-fbs"] = nlohmann::json::array();
    for (const auto& rtcpFeedback : payloadType.rtcpFeedbacks)
    {
        nlohmann::json rtcpFeedbackJson;
        rtcpFeedbackJson["type"] = rtcpFeedback.first;
        setIfExists(rtcpFeedbackJson, "subtype", rtcpFeedback.second);
        payloadTypeJson["rtcp-fbs"].push_back(rtcpFeedbackJson);
    }

    return payloadTypeJson;
}

nlohmann::json generateRtpHeaderExtensions(const std::vector<std::pair<uint32_t, std::string>>& rtpHeaderExtensions)
{
    auto rtpHeaderExtensionsJson = nlohmann::json::array();

    for (const auto& rtpHeaderExtension : rtpHeaderExtensions)
    {
        nlohmann::json rtpHeaderExtensionJson;
        rtpHeaderExtensionJson["id"] = rtpHeaderExtension.first;
        rtpHeaderExtensionJson["uri"] = rtpHeaderExtension.second;
        rtpHeaderExtensionsJson.push_back(rtpHeaderExtensionJson);
    }

    return rtpHeaderExtensionsJson;
}

} // namespace

namespace api
{

namespace Generator
{

nlohmann::json generateAllocateEndpointResponse(const EndpointDescription& channelsDescription)
{
    nlohmann::json responseJson;

    if (channelsDescription.bundleTransport.isSet())
    {
        responseJson["bundle-transport"] = generateTransport(channelsDescription.bundleTransport.get());
    }

    if (channelsDescription.audio.isSet())
    {
        const auto& audio = channelsDescription.audio.get();
        nlohmann::json audioJson;
        if (audio.transport.isSet())
        {
            audioJson["transport"] = generateTransport(audio.transport.get());
        }
        audioJson["ssrcs"] = nlohmann::json::array();
        for (const auto ssrc : audio.ssrcs)
        {
            audioJson["ssrcs"].push_back(ssrc);
        }

        audioJson["payload-types"] = nlohmann::json::array();
        for (const auto& payloadType : audio.payloadTypes)
        {
            audioJson["payload-types"].push_back(generatePayloadType(payloadType));
        }
        audioJson["rtp-hdrexts"] = generateRtpHeaderExtensions(audio.rtpHeaderExtensions);

        responseJson["audio"] = audioJson;
    }

    if (channelsDescription.video.isSet())
    {
        const auto& video = channelsDescription.video.get();
        nlohmann::json videoJson;
        if (video.transport.isSet())
        {
            videoJson["transport"] = generateTransport(video.transport.get());
        }

        videoJson["streams"] = nlohmann::json::array();
        for (const auto& stream : video.streams)
        {
            nlohmann::json streamJson;
            streamJson["sources"] = nlohmann::json::array();
            for (const auto level : stream.sources)
            {
                nlohmann::json sourceJson;
                sourceJson["main"] = level.main;
                if (level.feedback != 0)
                {
                    sourceJson["feedback"] = level.feedback;
                }
                streamJson["sources"].push_back(sourceJson);
            }
            streamJson["content"] = stream.content;
            videoJson["streams"].push_back(streamJson);
        }

        videoJson["payload-types"] = nlohmann::json::array();
        for (const auto& payloadType : video.payloadTypes)
        {
            videoJson["payload-types"].push_back(generatePayloadType(payloadType));
        }

        videoJson["rtp-hdrexts"] = generateRtpHeaderExtensions(video.rtpHeaderExtensions);

        responseJson["video"] = videoJson;
    }

    if (channelsDescription.data.isSet())
    {
        const auto& data = channelsDescription.data.get();
        nlohmann::json dataJson;
        dataJson["port"] = data.port;
        responseJson["data"] = dataJson;
    }

    return responseJson;
}

nlohmann::json generateConferenceEndpoint(const ConferenceEndpoint& endpoint)
{
    nlohmann::json jsonEndpoint = nlohmann::json::object();
    jsonEndpoint.emplace("id", endpoint.id);
    jsonEndpoint.emplace("isDominantSpeaker", endpoint.isDominantSpeaker);
    jsonEndpoint.emplace("isActiveTalker", endpoint.isActiveTalker);
    jsonEndpoint.emplace("iceState", api::utils::toString(endpoint.iceState));
    jsonEndpoint.emplace("dtlsState", api::utils::toString(endpoint.dtlsState));

    if (endpoint.isActiveTalker)
    {
        nlohmann::json activeTalkerInfo = nlohmann::json::object();
        activeTalkerInfo.emplace("ptt", endpoint.activeTalkerInfo.isPtt);
        activeTalkerInfo.emplace("score", endpoint.activeTalkerInfo.score);
        activeTalkerInfo.emplace("noiseLevel", endpoint.activeTalkerInfo.noiseLevel);
        jsonEndpoint.emplace("ActiveTalker", activeTalkerInfo);
    }
    return jsonEndpoint;
}

nlohmann::json generateExtendedConferenceEndpoint(const ConferenceEndpointExtendedInfo& endpoint)
{
    nlohmann::json jsonEndpoint = generateConferenceEndpoint(endpoint.basicEndpointInfo);
    {
        nlohmann::json fiveTuple = nlohmann::json::object();
        fiveTuple.emplace("localIP", endpoint.localIP);
        fiveTuple.emplace("localPort", endpoint.localPort);
        fiveTuple.emplace("protocol", endpoint.protocol);
        fiveTuple.emplace("remoteIP", endpoint.remoteIP);
        fiveTuple.emplace("remotePort", endpoint.remotePort);
        jsonEndpoint.emplace("iceSelectedTuple", fiveTuple);
    }
    {
        nlohmann::json ssrcMap = nlohmann::json::array();
        {
            nlohmann::json ssrcMsid = nlohmann::json::object();
            if (endpoint.userId.isSet())
            {
                nlohmann::json ssrcs = nlohmann::json::object();
                ssrcs.emplace("ssrcOriginal", endpoint.ssrcOriginal);
                ssrcs.emplace("ssrcRewritten", endpoint.ssrcRewritten);
                ssrcMsid.emplace(std::to_string(endpoint.userId.get()), ssrcs);
            }
            ssrcMap.push_back(ssrcMsid);
        }
        jsonEndpoint.emplace("audioUserIdToSsrcMap", ssrcMap);
    }
    return jsonEndpoint;
}

nlohmann::json generateAllocateBarbellResponse(const BarbellDescription& channelsDescription)
{
    nlohmann::json responseJson;

    responseJson["bundle-transport"] = generateTransport(channelsDescription.transport);

    const auto& audio = channelsDescription.audio;
    nlohmann::json audioJson;

    audioJson["ssrcs"] = nlohmann::json::array();
    for (const auto ssrc : audio.ssrcs)
    {
        audioJson["ssrcs"].push_back(ssrc);
    }
    audioJson["payload-types"] = nlohmann::json::array();
    for (const auto& payloadType : audio.payloadTypes)
    {
        audioJson["payload-types"].push_back(generatePayloadType(payloadType));
    }

    audioJson["rtp-hdrexts"] = generateRtpHeaderExtensions(audio.rtpHeaderExtensions);

    responseJson["audio"] = audioJson;

    const auto& video = channelsDescription.video;
    nlohmann::json videoJson;

    videoJson["streams"] = nlohmann::json::array();
    for (const auto& stream : video.streams)
    {
        nlohmann::json streamJson;
        streamJson["sources"] = nlohmann::json::array();
        for (const auto level : stream.sources)
        {
            nlohmann::json sourceJson;
            sourceJson["main"] = level.main;
            if (level.feedback != 0)
            {
                sourceJson["feedback"] = level.feedback;
            }
            streamJson["sources"].push_back(sourceJson);
        }
        streamJson["content"] = stream.content;
        videoJson["streams"].push_back(streamJson);

        videoJson["payload-types"] = nlohmann::json::array();
        for (const auto& payloadType : video.payloadTypes)
        {
            videoJson["payload-types"].push_back(generatePayloadType(payloadType));
        }

        videoJson["rtp-hdrexts"] = generateRtpHeaderExtensions(video.rtpHeaderExtensions);

        responseJson["video"] = videoJson;
    }

    const auto& data = channelsDescription.data;
    nlohmann::json dataJson;
    dataJson["port"] = data.port;
    responseJson["data"] = dataJson;

    return responseJson;
}

} // namespace Generator

} // namespace api
