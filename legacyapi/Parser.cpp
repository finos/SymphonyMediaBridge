#include "legacyapi/Parser.h"
#include "utils/Base64.h"

namespace
{

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

legacyapi::Transport parseTransport(const nlohmann::json& data)
{
    legacyapi::Transport transport;

    setIfExists(transport._xmlns, data, "xmlns");

    if (data.find("rtcp-mux") != data.end())
    {
        const auto& rtcpMux = data["rtcp-mux"];
        if (rtcpMux.is_string())
        {
            transport._rtcpMux = data["rtcp-mux"].get<std::string>().compare("true") == 0;
        }
        else
        {
            transport._rtcpMux = data["rtcp-mux"].get<bool>();
        }
    }
    else
    {
        transport._rtcpMux = false;
    }

    setIfExists(transport._ufrag, data, "ufrag");
    setIfExists(transport._pwd, data, "pwd");

    if (data.find("fingerprints") != data.end())
    {
        for (const auto& fingerprintJson : data["fingerprints"])
        {
            legacyapi::Fingerprint fingerprint;

            fingerprint._fingerprint = fingerprintJson["fingerprint"].get<std::string>();
            fingerprint._setup = fingerprintJson["setup"].get<std::string>();
            fingerprint._hash = fingerprintJson["hash"].get<std::string>();

            transport._fingerprints.emplace_back(std::move(fingerprint));
        }
    }

    if (data.find("candidates") != data.end())
    {
        for (const auto& candidateJson : data["candidates"])
        {
            legacyapi::Candidate candidate;

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

            transport._candidates.emplace_back(std::move(candidate));
        }
    }
    else if (data.find("connection") != data.end())
    {
        const auto& connectionJson = data["connection"];
        legacyapi::Connection connection;

        connection._port = connectionJson["port"].get<uint32_t>();
        connection._ip = connectionJson["ip"].get<std::string>();

        transport._connection.set(std::move(connection));
    }

    return transport;
}

} // namespace

namespace legacyapi
{

namespace Parser
{

Conference parse(const nlohmann::json& data)
{
    Conference conference;
    conference._id = data["id"].get<std::string>();

    if (data.find("channel-bundles") != data.end())
    {
        for (const auto& channelBundleJson : data["channel-bundles"])
        {
            ChannelBundle channelBundle;
            channelBundle._id = channelBundleJson["id"].get<std::string>();
            channelBundle._transport = parseTransport(channelBundleJson["transport"]);
            conference._channelBundles.emplace_back(std::move(channelBundle));
        }
    }

    if (data.find("contents") != data.end())
    {
        for (const auto& contentJson : data["contents"])
        {
            Content content;
            content._name = contentJson["name"].get<std::string>();

            if (contentJson.find("channels") != contentJson.end())
            {
                for (const auto& channelJson : contentJson["channels"])
                {
                    Channel channel;

                    setIfExists<>(channel._id, channelJson, "id");
                    setIfExists<>(channel._endpoint, channelJson, "endpoint");
                    setIfExists<>(channel._channelBundleId, channelJson, "channel-bundle-id");
                    setIfExists<>(channel._initiator, channelJson, "initiator");
                    setIfExists<>(channel._rtpLevelRelayType, channelJson, "rtp-level-relay-type");
                    setIfExists<>(channel._expire, channelJson, "expire");
                    setIfExists<>(channel._direction, channelJson, "direction");
                    setIfExists<>(channel._lastN, channelJson, "last-n");

                    if (channelJson.find("sources") != channelJson.end())
                    {
                        for (const auto& sourceJson : channelJson["sources"])
                        {
                            channel._sources.push_back(sourceJson.get<uint32_t>());
                        }
                    }

                    if (channelJson.find("transport") != channelJson.end())
                    {
                        channel._transport.set(parseTransport(channelJson["transport"]));
                    }

                    if (channelJson.find("payload-types") != channelJson.end())
                    {
                        for (const auto& payloadTypeJson : channelJson["payload-types"])
                        {
                            PayloadType payloadType;

                            payloadType._id = payloadTypeJson["id"].get<uint32_t>();
                            payloadType._name = payloadTypeJson["name"].get<std::string>();

                            {
                                const auto& clockRate = payloadTypeJson["clockrate"];
                                if (clockRate.is_string())
                                {
                                    payloadType._clockRate = payloadTypeJson["clockrate"].get<std::string>();
                                }
                                else
                                {
                                    payloadType._clockRate =
                                        std::to_string(payloadTypeJson["clockrate"].get<uint32_t>());
                                }
                            }

                            setIfExists(payloadType._channels, payloadTypeJson, "channels");

                            if (payloadTypeJson.find("parameters") != payloadTypeJson.end())
                            {
                                const auto& parametersJson = payloadTypeJson["parameters"];
                                for (auto parameterJsonItr = parametersJson.cbegin();
                                     parameterJsonItr != parametersJson.cend();
                                     ++parameterJsonItr)
                                {
                                    payloadType._parameters.emplace_back(
                                        std::make_pair(parameterJsonItr.key(), parameterJsonItr.value()));
                                }
                            }

                            if (payloadTypeJson.find("rtcp-fbs") != payloadTypeJson.end())
                            {
                                for (const auto& rtcpFbJson : payloadTypeJson["rtcp-fbs"])
                                {
                                    PayloadType::RtcpFb rtcpFb;
                                    rtcpFb._type = rtcpFbJson["type"].get<std::string>();
                                    setIfExists(rtcpFb._subtype, rtcpFbJson, "subtype");
                                    payloadType._rtcpFbs.emplace_back(std::move(rtcpFb));
                                }
                            }

                            channel._payloadTypes.emplace_back(std::move(payloadType));
                        }
                    }

                    if (channelJson.find("rtp-hdrexts") != channelJson.end())
                    {
                        for (const auto& rtpHdrExtJson : channelJson["rtp-hdrexts"])
                        {
                            Channel::RtpHdrExt rtpHdrExt;
                            rtpHdrExt._id = rtpHdrExtJson["id"].get<uint32_t>();
                            rtpHdrExt._uri = rtpHdrExtJson["uri"].get<std::string>();
                            channel._rtpHeaderHdrExts.emplace_back(std::move(rtpHdrExt));
                        }
                    }

                    if (channelJson.find("ssrc-groups") != channelJson.end())
                    {
                        for (const auto& ssrcGroupJson : channelJson["ssrc-groups"])
                        {
                            SsrcGroup ssrcGroup;
                            for (const auto& ssrcJson : ssrcGroupJson["sources"])
                            {
                                const uint32_t ssrc = ssrcJson.is_string() ? std::stoul(ssrcJson.get<std::string>())
                                                                           : ssrcJson.get<uint32_t>();
                                ssrcGroup._sources.push_back(ssrc);
                            }
                            ssrcGroup._semantics = ssrcGroupJson["semantics"].get<std::string>();
                            channel._ssrcGroups.emplace_back(std::move(ssrcGroup));
                        }
                    }

                    if (channelJson.find("ssrc-attributes") != channelJson.end())
                    {
                        for (const auto& ssrcAttributeJson : channelJson["ssrc-attributes"])
                        {
                            SsrcAttribute ssrcAttribute;
                            ssrcAttribute._content = ssrcAttributeJson["content"].get<std::string>();
                            for (const auto& ssrcJson : ssrcAttributeJson["sources"])
                            {
                                const auto ssrc = ssrcJson.is_string() ? std::stoul(ssrcJson.get<std::string>())
                                                                       : ssrcJson.get<uint32_t>();
                                ssrcAttribute._sources.push_back(ssrc);
                            }
                            channel._ssrcAttributes.push_back(ssrcAttribute);
                        }
                    }

                    if (channelJson.find("ssrc-whitelist") != channelJson.end())
                    {
                        std::vector<uint32_t> ssrcWhitelist;
                        for (const auto& ssrcJson : channelJson["ssrc-whitelist"])
                        {
                            const auto ssrc = ssrcJson.is_string() ? std::stoul(ssrcJson.get<std::string>())
                                                                   : ssrcJson.get<uint32_t>();
                            ssrcWhitelist.push_back(ssrc);
                        }
                        channel._ssrcWhitelist.set(std::move(ssrcWhitelist));
                    }

                    content._channels.emplace_back(std::move(channel));
                }
            }

            if (contentJson.find("sctpconnections") != contentJson.end())
            {
                for (const auto& sctpConnectionJson : contentJson["sctpconnections"])
                {
                    SctpConnection sctpConnection;

                    setIfExists<>(sctpConnection._id, sctpConnectionJson, "id");
                    setIfExists<>(sctpConnection._endpoint, sctpConnectionJson, "endpoint");
                    setIfExists<>(sctpConnection._channelBundleId, sctpConnectionJson, "channel-bundle-id");
                    setIfExists<>(sctpConnection._initiator, sctpConnectionJson, "initiator");
                    setIfExists<>(sctpConnection._expire, sctpConnectionJson, "expire");
                    setIfExists<>(sctpConnection._port, sctpConnectionJson, "port");

                    if (sctpConnectionJson.find("transport") != sctpConnectionJson.end())
                    {
                        sctpConnection._transport.set(parseTransport(sctpConnectionJson["transport"]));
                    }

                    content._sctpConnections.emplace_back(std::move(sctpConnection));
                }
            }

            conference._contents.emplace_back(std::move(content));
        }
    }

    if (data.find("recording") != data.end())
    {
        api::Recording recording;

        const auto& recordingJson = data["recording"];

        recording._recordingId = recordingJson["recording-id"].get<std::string>();
        recording._userId = recordingJson["user-id"].get<std::string>();

        const auto& modalaties = recordingJson["recording-modalities"];
        setIfExistsOrDefault<>(recording._isAudioEnabled, modalaties, "audio", false);
        setIfExistsOrDefault<>(recording._isVideoEnabled, modalaties, "video", false);
        setIfExistsOrDefault<>(recording._isScreenshareEnabled, modalaties, "screenshare", false);

        if (recordingJson.find("channels") != recordingJson.end())
        {
            for (const auto& channelJson : recordingJson["channels"])
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
                    utils::Base64::decode(aesKeyEnc, recordingChannel._aesKey, 32);
                }
                if (!saltEnc.empty())
                {
                    utils::Base64::decode(saltEnc, recordingChannel._aesSalt, 12);
                }

                recording._channels.emplace_back(recordingChannel);
            }
        }
        conference._recording.set(std::move(recording));
    }

    return conference;
}

} // namespace Parser

} // namespace legacyapi
