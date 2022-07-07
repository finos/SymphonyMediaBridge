#include "ApiChannel.h"
#include "HttpRequests.h"
#include "utils/IdGenerator.h"

namespace
{
std::string newGuuid()
{
    utils::IdGenerator idGen;
    char uuid[200];

    sprintf(uuid,
        "%08x-%04x-%04x-%04x-%012x",
        static_cast<uint32_t>(idGen.next() & 0xFFFFFFFFu),
        static_cast<uint32_t>(idGen.next() & 0xFFFFu),
        static_cast<uint32_t>(idGen.next() & 0xFFFFu),
        static_cast<uint32_t>(idGen.next() & 0xFFFFu),
        static_cast<uint32_t>(idGen.next()));

    return uuid;
}

std::string newIdString()
{
    utils::IdGenerator idGen;
    char uuid[200];

    sprintf(uuid, "%08u", static_cast<uint32_t>(idGen.next() & 0xFFFFFFFFu));

    return uuid;
}
} // namespace

namespace emulator
{

void Conference::create(const std::string& baseUrl)
{
    HttpPostRequest request((baseUrl + "/conferences").c_str(), "{\"last-n\":9}");
    request.awaitResponse(3000 * utils::Time::ms);

    if (request.isSuccess())
    {
        auto body = request.getJsonBody();
        _id = body["id"].get<std::string>();
        _success = true;
    }
}

BaseChannel::BaseChannel() : _id(newGuuid()), _audioId(newIdString()), _dataId(newIdString()), _videoId(newIdString())
{
}

void BaseChannel::setRemoteIce(transport::RtcTransport& transport,
    nlohmann::json bundle,
    const char* candidatesGroupName,
    memory::AudioPacketPoolAllocator& allocator)
{
    ice::IceCandidates candidates;

    for (auto& c : bundle[candidatesGroupName]["candidates"])
    {
        candidates.push_back(ice::IceCandidate(c["foundation"].template get<std::string>().c_str(),
            ice::IceComponent::RTP,
            c["protocol"] == "udp" ? ice::TransportType::UDP : ice::TransportType::TCP,
            c["priority"].template get<uint32_t>(),
            transport::SocketAddress::parse(c["ip"], c["port"]),
            ice::IceCandidate::Type::HOST));
    }

    std::pair<std::string, std::string> credentials;
    credentials.first = bundle[candidatesGroupName]["ufrag"];
    credentials.second = bundle[candidatesGroupName]["pwd"];

    transport.setRemoteIce(credentials, candidates, allocator);
}

void Channel::create(const std::string& baseUrl,
    const std::string& conferenceId,
    const bool initiator,
    const bool audio,
    const bool video,
    const bool forwardMedia)
{
    assert(!conferenceId.empty());
    _conferenceId = conferenceId;
    _relayType = forwardMedia ? "ssrc-rewrite" : "mixed";
    _baseUrl = baseUrl;
    _videoEnabled = video;

    using namespace nlohmann;
    json body = {{"action", "allocate"},
        {"bundle-transport", {{"ice-controlling", true}, {"ice", true}, {"dtls", true}, {"rtcp-mux", true}}}};

    if (audio)
    {
        body["audio"] = {{"relay-type", _relayType.c_str()}};
    }
    if (video)
    {
        body["video"] = {{"relay-type", "ssrc-rewrite"}};
    }
    if (audio || video)
    {
        body["data"] = json::object();
    }

    logger::debug("allocate ch with %s", "", body.dump().c_str());
    HttpPostRequest request((std::string(baseUrl) + "/conferences/" + conferenceId + "/" + _id).c_str(),
        body.dump().c_str());
    request.awaitResponse(9000 * utils::Time::ms);

    if (request.isSuccess())
    {
        _offer = request.getJsonBody();
        raw = request.getResponse();
    }
    else
    {
        logger::error("failed to allocate channel %d", "Test", request.getCode());
    }
}

void Channel::sendResponse(const std::pair<std::string, std::string>& iceCredentials,
    const ice::IceCandidates& candidates,
    const std::string& fingerprint,
    uint32_t audioSsrc,
    uint32_t* videoSsrcs)
{
    using namespace nlohmann;
    json body = {{"action", "configure"}};

    auto transportSpec = json::object({{"dtls", {{"setup", "active"}, {"type", "sha-256"}, {"hash", fingerprint}}},
        {"ice", {{"ufrag", iceCredentials.first}, {"pwd", iceCredentials.second}, {"candidates", json::array()}}}});

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

        transportSpec["ice"]["candidates"].push_back(jsonCandidate);
    }

    body["bundle-transport"] = transportSpec;

    if (audioSsrc != 0)
    {
        auto audioContent =
            json::object({{"rtp-hdrexts",
                              json::array({{{"id", 1}, {"uri", "urn:ietf:params:rtp-hdrext:ssrc-audio-level"}},
                                  {{"id", 3}, {"uri", "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"}}})},
                {"ssrcs", json::array({audioSsrc})},
                {"payload-type",
                    {{"id", 111},
                        {"name", "opus"},
                        {"clockrate", 48000},
                        {"channels", 2},
                        {"rtcp-fbs", json::array()}}}});

        body["audio"] = audioContent;
    }

    if (videoSsrcs)
    {
        auto videoContent = json::object();
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

        payloadTypes.push_back(json::object({{"id", 96},
            {"name", "rtx"},
            {"clockrate", "90000"},
            {"rtcp-fbs", json::array()},
            {"parameters", {{"apt", "100"}}}}));

        videoContent["payload-types"] = payloadTypes;
        auto sources = json::array();
        for (uint32_t* pSsrc = videoSsrcs; *pSsrc != 0; ++pSsrc)
        {
            sources.push_back(*pSsrc);
        }
        videoContent["ssrcs"] = sources;

        auto ssrcGroups = json::array();
        auto mainSsrcs = json::array();
        for (uint32_t* pSsrc = videoSsrcs; *pSsrc != 0; pSsrc += 2)
        {
            mainSsrcs.push_back(*pSsrc);
        }
        ssrcGroups.push_back(json::object({{"ssrcs", mainSsrcs}, {"semantics", "SIM"}}));

        for (uint32_t* pSsrc = videoSsrcs; *pSsrc != 0; pSsrc += 2)
        {
            ssrcGroups.push_back(json::object({{"ssrcs", json::array({*pSsrc, *(pSsrc + 1)})}, {"semantics", "FID"}}));
        }
        videoContent["ssrc-groups"] = ssrcGroups;

        videoContent["rtp-hdrexts"] =
            json::array({{{"id", 3}, {"uri", "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"}},
                {{"id", 4}, {"uri", "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"}}});

        body["video"] = videoContent;
    }

    body["data"] = json::object({{"port", 5000}});

    logger::info("patch channel with %s", "Test", body.dump().c_str());

    HttpPostRequest request((_baseUrl + "/conferences/" + _conferenceId + "/" + _id).c_str(), body.dump().c_str());
    request.awaitResponse(3000 * utils::Time::ms);

    if (request.isSuccess())
    {
        raw = request.getResponse();
    }
    else
    {
        logger::error("failed to patch channel %d", "Test", request.getCode());
    }
}

void Channel::configureTransport(transport::RtcTransport& transport, memory::AudioPacketPoolAllocator& allocator)
{
    auto bundle = _offer["bundle-transport"];
    setRemoteIce(transport, bundle, ICE_GROUP, allocator);

    std::string fingerPrint = bundle["dtls"]["hash"];
    transport.setRemoteDtlsFingerprint(bundle["dtls"]["type"], fingerPrint, true);
}

std::unordered_set<uint32_t> Channel::getOfferedVideoSsrcs() const
{
    std::unordered_set<uint32_t> ssrcs;
    if (_offer.find("video") != _offer.end())
    {
        for (uint32_t ssrc : _offer["video"]["ssrcs"])
        {
            ssrcs.emplace(ssrc);
        }
    }

    return ssrcs;
}

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

void ColibriChannel::create(const std::string& baseUrl,
    const std::string& conferenceId,
    const bool initiator,
    const bool audio,
    const bool video,
    const bool forwardMedia)
{
    _conferenceId = conferenceId;
    _relayType = forwardMedia ? "ssrc-rewrite" : "mixer";
    _baseUrl = baseUrl;
    _videoEnabled = video;

    using namespace nlohmann;
    json body = {{"id", conferenceId}, {"contents", json::array()}};

    if (audio)
    {
        body["contents"].push_back(newContent(_id, "audio", _relayType.c_str(), initiator));
    }
    if (video)
    {
        auto videoContent = newContent(_id, "video", "ssrc-rewrite", initiator);
        videoContent["channels"][0]["last-n"] = 5;
        body["contents"].push_back(videoContent);
    }
    if (audio || video)
    {
        // data must be last or request handler cannot decide if bundling is used
        body["contents"].push_back(json::object({{"name", "data"},
            {"sctpconnections",
                json::array({json::object(
                    {{"initiator", initiator}, {"endpoint", _id}, {"expire", 60}, {"channel-bundle-id", _id}})})}}));
    }

    logger::debug("allocate ch with %s", "", body.dump().c_str());
    HttpPatchRequest request((std::string(baseUrl) + "/colibri/conferences/" + conferenceId).c_str(),
        body.dump().c_str());
    request.awaitResponse(90000 * utils::Time::ms);

    if (request.isSuccess())
    {
        _offer = request.getJsonBody();
        raw = request.getResponse();
    }
    else
    {
        logger::error("failed to allocate channel %d", "Test", request.getCode());
    }
}

void ColibriChannel::sendResponse(const std::pair<std::string, std::string>& iceCredentials,
    const ice::IceCandidates& candidates,
    const std::string& fingerprint,
    uint32_t audioSsrc,
    uint32_t* videoSsrcs)
{
    using namespace nlohmann;
    json body = {{"id", _conferenceId},
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
                    {"rtp-level-relay-type", _relayType},
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
            {"rtp-level-relay-type", _relayType},
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

        payloadTypes.push_back(json::object({{"id", 96},
            {"name", "rtx"},
            {"clockrate", "90000"},
            {"rtcp-fbs", json::array()},
            {"parameters", {{"apt", "100"}}}}));

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

    HttpPatchRequest request((_baseUrl + "/colibri/conferences/" + _conferenceId).c_str(), body.dump().c_str());
    request.awaitResponse(3000 * utils::Time::ms);

    if (request.isSuccess())
    {
        raw = request.getResponse();
    }
    else
    {
        logger::error("failed to patch channel %d", "Test", request.getCode());
    }
}

void ColibriChannel::configureTransport(transport::RtcTransport& transport, memory::AudioPacketPoolAllocator& allocator)
{
    for (auto& bundle : _offer["channel-bundles"])
    {
        setRemoteIce(transport, bundle, TRANSPORT_GROUP, allocator);

        std::string fingerPrint = bundle["transport"]["fingerprints"][0]["fingerprint"];
        transport.setRemoteDtlsFingerprint(bundle["transport"]["fingerprints"][0]["hash"], fingerPrint, true);
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
} // namespace emulator
