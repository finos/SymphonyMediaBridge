#include "test/integration/emulator/ColibriChannel.h"
#include "test/integration/emulator/HttpRequests.h"

namespace emulator
{

nlohmann::json newContent(const std::string& endpointId, const char* type, const char* relayType, bool initiator)
{
    using namespace nlohmann;
    json contentItem = json::object({{"name", type},
        {"channels",
            json::array({json::object({{"initiator", initiator},
                {"endpoint", endpointId},
                {"expire", 60},
                {"direction", "sendrecv"},
                {"channel-bundle-id", endpointId},
                {"rtp-level-relay-type", relayType}})})}});

    return contentItem;
}

bool ColibriChannel::create(bool initiator, const CallConfig& config)
{
    _callConfig = config;
    if (_callConfig.relayType == "mixed")
    {
        _callConfig.relayType = "mixer";
    }
    // Colibri endpoints do not support idle timeouts.
    assert(0 == _callConfig.idleTimeout);

    using namespace nlohmann;
    json body = {{"id", _callConfig.conferenceId}, {"contents", json::array()}};

    if (_callConfig.audio)
    {
        body["contents"].push_back(newContent(_id, "audio", _callConfig.relayType.c_str(), initiator));
    }
    if (_callConfig.video)
    {
        auto videoContent = newContent(_id, "video", "ssrc-rewrite", initiator);
        videoContent["channels"][0]["last-n"] = 5;
        body["contents"].push_back(videoContent);
    }
    if (_callConfig.audio || _callConfig.video)
    {
        // data must be last or request handler cannot decide if bundling is used
        body["contents"].push_back(json::object({{"name", "data"},
            {"sctpconnections",
                json::array({json::object(
                    {{"initiator", initiator}, {"endpoint", _id}, {"expire", 60}, {"channel-bundle-id", _id}})})}}));
    }

    logger::debug("allocate ch with %s", "ApiChannel", body.dump().c_str());
    nlohmann::json responseBody;
    auto success = awaitResponse<HttpPatchRequest>(_httpd,
        std::string(_callConfig.baseUrl) + "/colibri/conferences/" + _callConfig.conferenceId,
        body.dump(),
        90 * utils::Time::sec,
        responseBody);

    if (success)
    {
        _offer = responseBody;
        return true;
    }
    else
    {
        logger::error("failed to allocate channel", "ApiChannel");
        return false;
    }
}

bool ColibriChannel::sendResponse(transport::RtcTransport& bundleTransport,
    const std::string& fingerprint,
    uint32_t audioSsrc,
    uint32_t* videoSsrcs)
{
    using namespace nlohmann;

    auto iceCredentials = bundleTransport.getLocalIceCredentials();
    auto candidates = bundleTransport.getLocalCandidates();

    json body = {{"id", _callConfig.conferenceId},
        {"contents", json::array()},
        {"channel-bundles", json::array({json::object({{"id", _id}})})}};

    auto transportSpec = json::object({{"xmlns", "urn:xmpp:jingle:transports:ice-udp:1"},
        {"rtcp-mux", true},
        {"ufrag", iceCredentials.first},
        {"pwd", iceCredentials.second},
        {"fingerprints", json::array({{{"setup", "active"}, {"hash", "sha-256"}, {"fingerprint", fingerprint}}})},
        {"candidates", json::array()}});

    for (auto& c : candidates)
    {
        auto jsonCandidate = json::object({{"foundation", c.getFoundation()},
            {"component", c.component},
            {"protocol", c.transportType == ice::TransportType::UDP ? "udp" : "tcp"},
            {"priority", c.priority},
            {"ip", c.address.ipToString()},
            {"port", c.address.getPort()},
            {"type", "host"},
            {"generation", 0},
            {"network", 1}});

        transportSpec["candidates"].push_back(jsonCandidate);
    }

    body["channel-bundles"][0]["transport"] = transportSpec;

    if (audioSsrc != 0)
    {
        auto audioContent = json::object({{"name", "audio"},
            {"channels",
                json::array({json::object({{"endpoint", _id},
                    {"expire", 180},
                    {"id", _audioId},
                    {"channel-bundle-id", _id},
                    {"rtp-level-relay-type", _callConfig.relayType},
                    {"direction", "sendrecv"},
                    {"rtp-hdrexts",
                        json::array({{{"id", 1}, {"uri", "urn:ietf:params:rtp-hdrext:ssrc-audio-level"}},
                            {{"id", 3},
                                {"uri",
                                    "http://www.webrtc.org/experiments/rtp-hdrext/"
                                    "abs-send-time"}}})}})})}});

        auto payloadTypesJson = json::array();
        payloadTypesJson.push_back(json::object({{"id", 111},
            {"parameters", {{"minptime", "10"}}},
            {"rtcp-fbs", json::array()},
            {"name", "opus"},
            {"clockrate", "48000"},
            {"channels", "2"}}));

        payloadTypesJson.push_back(json::object({{"id", 0},
            {"parameters", json::object()},
            {"rtcp-fbs", json::array()},
            {"name", "PCMU"},
            {"clockrate", "8000"}}));

        payloadTypesJson.push_back(json::object({{"id", 8},
            {"parameters", json::object()},
            {"rtcp-fbs", json::array()},
            {"name", "PCMA"},
            {"clockrate", "8000"}}));

        audioContent["channels"][0]["payload-types"] = payloadTypesJson;
        audioContent["channels"][0]["sources"] = json::array({audioSsrc});

        body["contents"].push_back(audioContent);
    }

    if (videoSsrcs)
    {
        auto videoContent = json::object({{"name", "video"}, {"channels", json::array()}});
        auto videoChannel = json::object({{"endpoint", _id},
            {"expire", 180},
            {"id", _videoId},
            {"channel-bundle-id", _id},
            {"rtp-level-relay-type", _callConfig.relayType},
            {"direction", "sendrecv"},
            {"rtp-hdrexts",
                json::array({{{"id", 3}, {"uri", "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"}},
                    {{"id", 4}, {"uri", "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"}}})}});
        auto payloadTypes = json::array();
        payloadTypes.push_back(json::object({{"id", 100},
            {"name", "VP8"},
            {"clockrate", "90000"},
            {"parameters", json::object()},
            {"rtcp-fbs",
                json::array({{{"type", "goog-remb"}},
                    {{"type", "ccm"}, {"subtype", "fir"}},
                    {{"type", "nack"}},
                    {{"type", "nack"}, {"subtype", "pli"}}})}}));

        if (_callConfig.rtx)
        {
            payloadTypes.push_back(json::object({{"id", 96},
                {"name", "rtx"},
                {"clockrate", "90000"},
                {"rtcp-fbs", json::array()},
                {"parameters", {{"apt", "100"}}}}));
        }

        videoChannel["payload-types"] = payloadTypes;

        auto sources = json::array();
        for (uint32_t* pSsrc = videoSsrcs; *pSsrc != 0; ++pSsrc)
        {
            sources.push_back(*pSsrc);
        }
        videoChannel["sources"] = sources;

        auto ssrcGroups = json::array();
        auto mainSsrcs = json::array();
        for (uint32_t* pSsrc = videoSsrcs; *pSsrc != 0; pSsrc += 2)
        {
            mainSsrcs.push_back(*pSsrc);
            ssrcGroups.push_back(
                json::object({{"sources", json::array({*pSsrc, *(pSsrc + 1)})}, {"semantics", "FID"}}));
        }
        ssrcGroups.push_back(json::object({{"semantics", "SIM"}, {"sources", mainSsrcs}}));
        videoChannel["ssrc-groups"] = ssrcGroups;

        videoContent["channels"].push_back(videoChannel);
        body["contents"].push_back(videoContent);
    }

    auto dataJson = json::object({{"name", "data"},
        {"sctpconnections",
            json::array({json::object({{"endpoint", _id},
                {"expire", 180},
                {"id", _dataId},
                {"channel-bundle-id", _id},
                {"port", "5000"},
                {"direction", "sendrecv"}})})}});
    body["contents"].push_back(dataJson);

    nlohmann::json responseBody;
    auto success = awaitResponse<HttpPatchRequest>(_httpd,
        _callConfig.baseUrl + "/colibri/conferences/" + _callConfig.conferenceId,
        body.dump(),
        3 * utils::Time::sec,
        responseBody);

    if (success)
    {
        _offer = responseBody;
    }
    else
    {
        logger::error("failed to patch channel", "ColibriChannel");
    }

    return success;
}

void ColibriChannel::disconnect()
{
    using namespace nlohmann;
    json body = {{"id", _callConfig.conferenceId},
        {"contents", json::array()},
        {"channel-bundles", json::array({json::object({{"id", _id}, {"transport", json::object()}})})}};

    auto audioContent = json::object({{"name", "audio"},
        {"channels", json::array({json::object({{"endpoint", _id}, {"expire", 0}, {"id", _audioId}})})}});
    body["contents"].push_back(audioContent);

    auto videoContent = json::object({{"name", "video"}, {"channels", json::array()}});
    auto videoChannel = json::object({{"endpoint", _id}, {"expire", 0}, {"id", _videoId}});
    videoContent["channels"].push_back(videoChannel);
    body["contents"].push_back(videoContent);

    logger::debug("expire %s", "ColibriChannel", body.dump().c_str());

    nlohmann::json responseBody;
    auto success = awaitResponse<HttpPatchRequest>(_httpd,
        _callConfig.baseUrl + "/colibri/conferences/" + _callConfig.conferenceId,
        body.dump(),
        3 * utils::Time::sec,
        responseBody);

    if (!success)
    {
        logger::error("failed to expire channel ", "ColibriChannel");
    }
}

void ColibriChannel::configureTransport(transport::RtcTransport& transport, memory::AudioPacketPoolAllocator& allocator)
{
    for (auto& bundle : _offer["channel-bundles"])
    {
        setRemoteIce(transport, bundle, TRANSPORT_GROUP, allocator);

        std::string fingerPrint = bundle["transport"]["fingerprints"][0]["fingerprint"];
        transport.asyncSetRemoteDtlsFingerprint(bundle["transport"]["fingerprints"][0]["hash"], fingerPrint, true);
    }
}

bool ColibriChannel::isAudioOffered() const
{
    for (auto& content : _offer["contents"])
    {
        if (content["name"] == "audio")
        {
            return true;
        }
    }
    return false;
}

std::unordered_set<uint32_t> ColibriChannel::getOfferedVideoSsrcs() const
{
    std::unordered_set<uint32_t> ssrcs;
    for (auto& content : _offer["contents"])
    {
        if (content["name"] == "video")
        {
            auto channel = content["channels"][0];
            for (uint32_t ssrc : channel["sources"])
            {
                ssrcs.emplace(ssrc);
            }
            return ssrcs;
        }
    }
    return ssrcs;
}

std::vector<api::SimulcastGroup> ColibriChannel::getOfferedVideoStreams() const
{
    std::vector<api::SimulcastGroup> v;

    for (auto& content : _offer["contents"])
    {
        if (content["name"] == "video")
        {
            auto channel = content["channels"][0];
            for (auto& jsonGroup : channel["ssrc-groups"])
            {
                api::SimulcastGroup group;
                api::SsrcPair ssrcPair{0};
                int index = 0;
                for (auto& ssrc : jsonGroup["sources"])
                {
                    if (index & 1)
                    {
                        ssrcPair.feedback = ssrc.get<uint32_t>();
                        group.push_back(ssrcPair);
                        v.push_back(group);
                    }
                    else
                    {
                        ssrcPair.main = ssrc.get<uint32_t>();
                    }
                    ++index;
                }
            }
            return v;
        }
    }

    return v;
}

utils::Optional<uint32_t> ColibriChannel::getOfferedScreensharingSsrc() const
{
    for (auto& content : _offer["contents"])
    {
        if (content["name"] == "video")
        {
            auto channel = content["channels"][0];

            if (channel.find("ssrc-attributes") != channel.end())
            {
                for (auto& attribute : channel["ssrc-attributes"])
                {
                    if (attribute["content"] == "slides")
                    {
                        return utils::Optional<uint32_t>(attribute["sources"][0].get<uint32_t>());
                    }
                }
            }
        }
    }

    return utils::Optional<uint32_t>();
}

utils::Optional<uint32_t> ColibriChannel::getOfferedLocalSsrc() const
{
    for (auto& content : _offer["contents"])
    {
        if (content["name"] == "video")
        {
            auto channel = content["channels"][0];
            return utils::Optional<uint32_t>(channel["sources"][0].get<uint32_t>());
        }
    }

    return utils::Optional<uint32_t>();
}

} // namespace emulator
