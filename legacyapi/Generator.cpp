#include "legacyapi/Generator.h"
#include "legacyapi/Conference.h"

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

nlohmann::json generateTransport(const legacyapi::Transport& transport)
{
    nlohmann::json transportJson;

    setIfExists(transportJson, "xmlns", transport._xmlns);
    setIfExists(transportJson, "ufrag", transport._ufrag);
    setIfExists(transportJson, "pwd", transport._pwd);
    transportJson["rtcp-mux"] = transport._rtcpMux;

    if (!transport._fingerprints.empty())
    {
        transportJson["fingerprints"] = nlohmann::json::array();
        for (const auto& fingerprint : transport._fingerprints)
        {
            nlohmann::json fingerprintJson;
            fingerprintJson["fingerprint"] = fingerprint._fingerprint;
            fingerprintJson["setup"] = fingerprint._setup;
            fingerprintJson["hash"] = fingerprint._hash;
            transportJson["fingerprints"].push_back(fingerprintJson);
        }
    }

    if (!transport._candidates.empty())
    {
        transportJson["candidates"] = nlohmann::json::array();
        for (const auto& candidate : transport._candidates)
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
            transportJson["candidates"].push_back(candidateJson);
        }
    }
    else if (transport._connection.isSet())
    {
        nlohmann::json connectionJson;
        connectionJson["port"] = transport._connection.get()._port;
        connectionJson["ip"] = transport._connection.get()._ip;
        transportJson["connection"] = connectionJson;
    }

    return transportJson;
}

} // namespace

namespace legacyapi
{

namespace Generator
{

nlohmann::json generate(const Conference& conference)
{
    nlohmann::json result;

    result["id"] = conference._id;

    if (!conference._channelBundles.empty())
    {
        result["channel-bundles"] = nlohmann::json::array();
        for (const auto& channelBundle : conference._channelBundles)
        {
            nlohmann::json channelBundleJson;
            channelBundleJson["id"] = channelBundle._id;
            channelBundleJson["transport"] = generateTransport(channelBundle._transport);
            result["channel-bundles"].push_back(channelBundleJson);
        }
    }

    if (!conference._contents.empty())
    {
        result["contents"] = nlohmann::json::array();
        for (const auto& content : conference._contents)
        {
            nlohmann::json contentJson;
            contentJson["name"] = content._name;
            if (content._name.compare("data") == 0)
            {
                contentJson["sctpconnections"] = nlohmann::json::array();
                for (const auto& sctpConnection : content._sctpConnections)
                {
                    nlohmann::json sctpConnectionJson;
                    setIfExists(sctpConnectionJson, "id", sctpConnection._id);
                    setIfExists(sctpConnectionJson, "endpoint", sctpConnection._endpoint);
                    setIfExists(sctpConnectionJson, "channel-bundle-id", sctpConnection._channelBundleId);
                    setIfExists(sctpConnectionJson, "initiator", sctpConnection._initiator);
                    setIfExists(sctpConnectionJson, "expire", sctpConnection._expire);
                    setIfExists(sctpConnectionJson, "port", sctpConnection._port);

                    if (sctpConnection._transport.isSet())
                    {
                        sctpConnectionJson["transport"] = generateTransport(sctpConnection._transport.get());
                    }
                    contentJson["sctpconnections"].push_back(sctpConnectionJson);
                }
            }
            else
            {
                contentJson["channels"] = nlohmann::json::array();
                for (const auto& channel : content._channels)
                {
                    nlohmann::json channelJson;
                    setIfExists(channelJson, "id", channel._id);
                    setIfExists(channelJson, "endpoint", channel._endpoint);
                    setIfExists(channelJson, "channel-bundle-id", channel._channelBundleId);
                    setIfExists(channelJson, "initiator", channel._initiator);
                    setIfExists(channelJson, "rtp-level-relay-type", channel._rtpLevelRelayType);
                    setIfExists(channelJson, "expire", channel._expire);
                    setIfExists(channelJson, "direction", channel._direction);
                    setIfExists(channelJson, "last-n", channel._lastN);

                    if (channel._transport.isSet())
                    {
                        channelJson["transport"] = generateTransport(channel._transport.get());
                    }

                    if (!channel._sources.empty())
                    {
                        channelJson["sources"] = nlohmann::json::array();
                        for (const auto source : channel._sources)
                        {
                            channelJson["sources"].push_back(source);
                        }
                    }

                    if (!channel._ssrcGroups.empty())
                    {
                        channelJson["ssrc-groups"] = nlohmann::json::array();
                        for (const auto& group : channel._ssrcGroups)
                        {
                            nlohmann::json groupJson;
                            groupJson["sources"] = nlohmann::json::array();
                            for (const auto ssrc : group._sources)
                            {
                                groupJson["sources"].push_back(ssrc);
                            }
                            groupJson["semantics"] = "FID";
                            channelJson["ssrc-groups"].push_back(groupJson);
                        }
                    }

                    if (!channel._ssrcAttributes.empty())
                    {
                        channelJson["ssrc-attributes"] = nlohmann::json::array();
                        for (const auto& attribute : channel._ssrcAttributes)
                        {
                            nlohmann::json attributeJson;
                            attributeJson["sources"] = nlohmann::json::array();
                            for (const auto ssrc : attribute._sources)
                            {
                                attributeJson["sources"].push_back(ssrc);
                            }
                            attributeJson["content"] = attribute._content;
                            channelJson["ssrc-attributes"].push_back(attributeJson);
                        }
                    }

                    if (!channel._payloadTypes.empty())
                    {
                        channelJson["payload-types"] = nlohmann::json::array();
                        for (const auto& payloadType : channel._payloadTypes)
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
                            for (const auto& rtcpFb : payloadType._rtcpFbs)
                            {
                                nlohmann::json rtcpFbJson;
                                rtcpFbJson["type"] = rtcpFb._type;
                                setIfExists(rtcpFbJson, "subtype", rtcpFb._subtype);
                                payloadTypeJson["rtcp-fbs"].push_back(rtcpFbJson);
                            }

                            channelJson["payload-types"].push_back(payloadTypeJson);
                        }
                    }

                    if (!channel._rtpHeaderHdrExts.empty())
                    {
                        channelJson["rtp-hdrexts"] = nlohmann::json::array();
                        for (const auto& rtpHdrExt : channel._rtpHeaderHdrExts)
                        {
                            nlohmann::json rtpHdrExtJson;
                            rtpHdrExtJson["id"] = rtpHdrExt._id;
                            rtpHdrExtJson["uri"] = rtpHdrExt._uri;
                            channelJson["rtp-hdrexts"].push_back(rtpHdrExtJson);
                        }
                    }

                    contentJson["channels"].push_back(channelJson);
                }
            }
            result["contents"].push_back(contentJson);
        }
    }

    return result;
}

} // namespace Generator

} // namespace legacyapi
