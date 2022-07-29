#include "api/Generator.h"
#include "api/ConferenceEndpoint.h"
#include "api/EndpointDescription.h"
#include "api/utils.h"

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

nlohmann::json generateTransport(const api::EndpointDescription::Transport& transport)
{
    nlohmann::json transportJson;
    transportJson["rtcp-mux"] = transport._rtcpMux;

    if (transport._ice.isSet())
    {
        const auto& ice = transport._ice.get();
        nlohmann::json iceJson;
        iceJson["ufrag"] = ice._ufrag;
        iceJson["pwd"] = ice._pwd;

        iceJson["candidates"] = nlohmann::json::array();
        for (const auto& candidate : ice._candidates)
        {
            nlohmann::json candidateJson;
            candidateJson["generation"] = candidate._generation;
            candidateJson["component"] = candidate._component;
            candidateJson["protocol"] = candidate._protocol;
            candidateJson["port"] = candidate._port;
            candidateJson["ip"] = candidate._ip;
            candidateJson["foundation"] = candidate._foundation;
            candidateJson["priority"] = candidate._priority;
            candidateJson["type"] = candidate._type;
            candidateJson["network"] = candidate._network;
            setIfExists(candidateJson, "rel-port", candidate._relPort);
            setIfExists(candidateJson, "rel-addr", candidate._relAddr);
            iceJson["candidates"].push_back(candidateJson);
        }

        transportJson["ice"] = iceJson;
    }

    if (transport._dtls.isSet())
    {
        const auto& dtls = transport._dtls.get();
        nlohmann::json dtlsJson;
        dtlsJson["type"] = dtls.type;
        dtlsJson["hash"] = dtls.hash;
        dtlsJson["setup"] = dtls.setup;
        transportJson["dtls"] = dtlsJson;
    }

    if (transport._connection.isSet())
    {
        nlohmann::json connectionJson;
        connectionJson["port"] = transport._connection.get()._port;
        connectionJson["ip"] = transport._connection.get()._ip;
        transportJson["connection"] = connectionJson;
    }

    return transportJson;
}

nlohmann::json generatePayloadType(const api::EndpointDescription::PayloadType& payloadType)
{
    nlohmann::json payloadTypeJson;

    payloadTypeJson["id"] = payloadType._id;
    payloadTypeJson["name"] = payloadType._name;
    payloadTypeJson["clockrate"] = payloadType._clockRate;
    setIfExists(payloadTypeJson, "channels", payloadType._channels);

    payloadTypeJson["parameters"] = nlohmann::json::object();

    for (const auto& parameter : payloadType._parameters)
    {
        payloadTypeJson["parameters"][parameter.first] = parameter.second;
    }

    payloadTypeJson["rtcp-fbs"] = nlohmann::json::array();
    for (const auto& rtcpFeedback : payloadType._rtcpFeedbacks)
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

    if (channelsDescription._bundleTransport.isSet())
    {
        responseJson["bundle-transport"] = generateTransport(channelsDescription._bundleTransport.get());
    }

    if (channelsDescription._audio.isSet())
    {
        const auto& audio = channelsDescription._audio.get();
        nlohmann::json audioJson;
        if (audio._transport.isSet())
        {
            audioJson["transport"] = generateTransport(audio._transport.get());
        }
        audioJson["ssrcs"] = nlohmann::json::array();
        for (const auto ssrc : audio._ssrcs)
        {
            audioJson["ssrcs"].push_back(ssrc);
        }
        if (audio._payloadType.isSet())
        {
            audioJson["payload-type"] = generatePayloadType(audio._payloadType.get());
        }
        audioJson["rtp-hdrexts"] = generateRtpHeaderExtensions(audio._rtpHeaderExtensions);

        responseJson["audio"] = audioJson;
    }

    if (channelsDescription._video.isSet())
    {
        const auto& video = channelsDescription._video.get();
        nlohmann::json videoJson;
        if (video._transport.isSet())
        {
            videoJson["transport"] = generateTransport(video._transport.get());
        }

        videoJson["ssrcs"] = nlohmann::json::array();
        for (const auto ssrc : video._ssrcs)
        {
            videoJson["ssrcs"].push_back(ssrc);
        }

        videoJson["ssrc-groups"] = nlohmann::json::array();
        for (const auto& ssrcGroup : video._ssrcGroups)
        {
            nlohmann::json ssrcGroupJson;
            ssrcGroupJson["ssrcs"] = nlohmann::json::array();
            for (const auto ssrc : ssrcGroup._ssrcs)
            {
                ssrcGroupJson["ssrcs"].push_back(ssrc);
            }
            ssrcGroupJson["semantics"] = ssrcGroup._semantics;

            videoJson["ssrc-groups"].push_back(ssrcGroupJson);
        }

        videoJson["payload-types"] = nlohmann::json::array();
        for (const auto& payloadType : video._payloadTypes)
        {
            videoJson["payload-types"].push_back(generatePayloadType(payloadType));
        }

        videoJson["rtp-hdrexts"] = generateRtpHeaderExtensions(video._rtpHeaderExtensions);

        videoJson["ssrc-attributes"] = nlohmann::json::array();
        for (const auto& ssrcAttribute : video._ssrcAttributes)
        {
            nlohmann::json ssrcAttributeJson;
            ssrcAttributeJson["ssrcs"] = nlohmann::json::array();
            for (const auto ssrc : ssrcAttribute._ssrcs)
            {
                ssrcAttributeJson["ssrcs"].push_back(ssrc);
            }
            ssrcAttributeJson["content"] = ssrcAttribute._content;
            videoJson["ssrc-attributes"].push_back(ssrcAttributeJson);
        }

        responseJson["video"] = videoJson;
    }

    if (channelsDescription._data.isSet())
    {
        const auto& data = channelsDescription._data.get();
        nlohmann::json dataJson;
        dataJson["port"] = data._port;
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

            ssrcMsid.emplace(std::to_string(endpoint.usid), std::to_string(endpoint.ssrc));
            ssrcMap.push_back(ssrcMsid);
        }
        jsonEndpoint.emplace("audioSsrcMap", ssrcMap);
    }
    return jsonEndpoint;
}

} // namespace Generator

} // namespace api
