#include "ApiChannel.h"
#include "HttpRequests.h"
#include "test/integration/emulator/Httpd.h"
#include "utils/Format.h"
#include "utils/IdGenerator.h"
#include "utils/StringBuilder.h"

namespace
{
std::string newGuuid()
{
    utils::IdGenerator idGen;
    std::string uuid(36, '\0');

    snprintf(&uuid.front(), // + null terminator
        uuid.size() + 1,
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
    std::string uuid(8, '\0');

    snprintf(&uuid.front(),
        uuid.size() + 1, // + null terminator
        "%08u",
        static_cast<uint32_t>(idGen.next() & 0xFFFFFFFFu));

    return uuid;
}
} // namespace

namespace emulator
{

void Conference::create(const std::string& baseUrl, bool useGlobalPort)
{
    assert(_success == false);

    nlohmann::json responseBody;
    nlohmann::json requestBody = {{"last-n", 9}, {"global-port", useGlobalPort}};

    _success = awaitResponse<HttpPostRequest>(_httpd,
        baseUrl + "/conferences",
        requestBody.dump().c_str(),
        3 * utils::Time::sec,
        responseBody);

    if (_success)
    {
        _id = responseBody["id"].get<std::string>();
    }
}

BaseChannel::BaseChannel(emulator::HttpdFactory* httpd)
    : _httpd(httpd),
      _id(newGuuid()),
      _audioId(newIdString()),
      _dataId(newIdString()),
      _videoId(newIdString())
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
    const bool forwardMedia,
    const uint32_t idleTimeout,
    const utils::Span<std::string> neighbours)
{
    assert(!conferenceId.empty());
    _conferenceId = conferenceId;
    _relayType = forwardMedia ? "ssrc-rewrite" : "mixed";
    _baseUrl = baseUrl;
    _videoEnabled = video;

    for (auto& n : neighbours)
    {
        _answerOptions.neighbours.push_back(n);
    }

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
    if (idleTimeout)
    {
        body["idleTimeout"] = idleTimeout;
    }

    logger::debug("allocate ch with %s", "ApiChannel", body.dump().c_str());
    nlohmann::json responseBody;
    auto success = awaitResponse<HttpPostRequest>(_httpd,
        std::string(baseUrl) + "/conferences/" + conferenceId + "/" + _id,
        body.dump(),
        9 * utils::Time::sec,
        responseBody);

    if (success)
    {
        _offer = responseBody;
        logger::debug("allocate offer received %s", "ApiChannel", responseBody.dump().c_str());
    }
    else
    {
        logger::error("failed to allocate channel", "ApiChannel");
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
                                  {{"id", 3}, {"uri", "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"}},
                                  {{"id", 8}, {"uri", "c9:params:rtp-hdrext:info"}}})},
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
            {"clockrate", 90000},
            {"parameters", json::object()},
            {"rtcp-fbs",
                json::array({{{"type", "goog-remb"}},
                    {{"type", "ccm"}, {"subtype", "fir"}},
                    {{"type", "nack"}},
                    {{"type", "nack"}, {"subtype", "pli"}}})}}));

        if (!_answerOptions.rtxDisabled)
        {
            payloadTypes.push_back(json::object({{"id", 96},
                {"name", "rtx"},
                {"clockrate", 90000},
                {"rtcp-fbs", json::array()},
                {"parameters", {{"apt", "100"}}}}));
        }

        videoContent["payload-types"] = payloadTypes;

        auto streamsArray = json::array();
        auto stream = json::object();
        auto sources = json::array();
        for (uint32_t* pSsrc = videoSsrcs; *pSsrc != 0; pSsrc += 2)
        {
            sources.push_back(json::object({{"main", *pSsrc}, {"feedback", *(pSsrc + 1)}}));
        }
        stream["sources"] = sources;
        stream["id"] = "msidX";
        stream["content"] = "video";
        streamsArray.push_back(stream);
        videoContent["streams"] = streamsArray;

        videoContent["rtp-hdrexts"] =
            json::array({{{"id", 3}, {"uri", "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"}},
                {{"id", 4}, {"uri", "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"}}});

        body["video"] = videoContent;
    }

    body["data"] = json::object({{"port", 5000}});

    if (!_answerOptions.neighbours.empty())
    {
        auto neighbours = json::object();
        auto groups = json::array();
        neighbours["action"] = "mute";
        for (auto& id : _answerOptions.neighbours)
        {
            groups.push_back(id);
        }
        neighbours["groups"] = groups;

        body["neighbours"] = neighbours;
    }

    logger::info("patch channel with %s", "ApiChannel", body.dump().c_str());

    nlohmann::json responseBody;
    auto success = awaitResponse<HttpPostRequest>(_httpd,
        _baseUrl + "/conferences/" + _conferenceId + "/" + _id,
        body.dump(),
        3 * utils::Time::sec,
        responseBody);

    if (success)
    {
        raw = responseBody.dump();
    }
    else
    {
        logger::error("failed to patch channel ", "ApiChannel");
    }
}

void Channel::disconnect()
{
    nlohmann::json responseBody;
    auto success = awaitResponse<HttpDeleteRequest>(_httpd,
        _baseUrl + "/conferences/" + _conferenceId + "/" + _id,
        3 * utils::Time::sec,
        responseBody);

    if (!success)
    {
        logger::error("failed to delete channel ", "ApiChannel");
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
        for (auto& stream : _offer["video"]["streams"])
        {
            for (auto& ssrcLevel : stream["sources"])
            {
                ssrcs.emplace(ssrcLevel["main"].get<uint32_t>());
                if (ssrcLevel.find("feedback") != ssrcLevel.end())
                {
                    ssrcs.emplace(ssrcLevel["feedback"].get<uint32_t>());
                }
            }
        }
    }

    return ssrcs;
}

std::vector<api::SimulcastGroup> Channel::getOfferedVideoStreams() const
{
    std::vector<api::SimulcastGroup> v;

    if (_offer.find("video") != _offer.end())
    {
        for (auto& stream : _offer["video"]["streams"])
        {
            api::SimulcastGroup group;
            auto content = stream["content"].get<std::string>();
            if (content.compare("local") == 0)
            {
                for (auto& ssrcPair : stream["sources"])
                {
                    api::SsrcPair ssrc = {ssrcPair["main"].get<uint32_t>(), 0};
                    group.push_back(ssrc);
                }
            }
            else if (content.compare("slides") == 0 || content.compare("video") == 0)
            {
                for (auto& ssrcPair : stream["sources"])
                {
                    api::SsrcPair ssrc = {ssrcPair["main"].get<uint32_t>(), ssrcPair["feedback"].get<uint32_t>()};
                    group.push_back(ssrc);
                }
            }

            v.push_back(group);
        }
    }

    return v;
}

utils::Optional<uint32_t> Channel::getOfferedScreensharingSsrc() const
{
    if (_offer.find("video") != _offer.end())
    {
        for (auto& stream : _offer["video"]["streams"])
        {
            api::SimulcastGroup group;
            auto content = stream["content"].get<std::string>();
            if (content.compare("slides") == 0)
            {
                for (auto& ssrcPair : stream["sources"])
                {
                    return utils::Optional<uint32_t>(ssrcPair["main"].get<uint32_t>());
                }
            }
        }
    }

    return utils::Optional<uint32_t>();
}

utils::Optional<uint32_t> Channel::getOfferedLocalSsrc() const
{
    if (_offer.find("video") != _offer.end())
    {
        for (auto& stream : _offer["video"]["streams"])
        {
            api::SimulcastGroup group;
            auto content = stream["content"].get<std::string>();
            if (content.compare("local") == 0)
            {
                for (auto& ssrcPair : stream["sources"])
                {
                    return utils::Optional<uint32_t>(ssrcPair["main"].get<uint32_t>());
                }
            }
        }
    }

    return utils::Optional<uint32_t>();
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
    const bool forwardMedia,
    const uint32_t idleTimeout,
    const utils::Span<std::string> neighbours)
{
    // Colibri endpoints do not support idle timeouts.
    assert(0 == idleTimeout);

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

    logger::debug("allocate ch with %s", "ApiChannel", body.dump().c_str());
    nlohmann::json responseBody;
    auto success = awaitResponse<HttpPatchRequest>(_httpd,
        std::string(baseUrl) + "/colibri/conferences/" + conferenceId,
        body.dump(),
        90 * utils::Time::sec,
        responseBody);

    if (success)
    {
        _offer = responseBody;
    }
    else
    {
        logger::error("failed to allocate channel", "ApiChannel");
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

        if (!_answerOptions.rtxDisabled)
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
        _baseUrl + "/colibri/conferences/" + _conferenceId,
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
}

void ColibriChannel::disconnect()
{
    using namespace nlohmann;
    json body = {{"id", _conferenceId},
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
        _baseUrl + "/colibri/conferences/" + _conferenceId,
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

Barbell::Barbell(emulator::HttpdFactory* httpd) : _httpd(httpd), _id(newIdString()) {}

std::string Barbell::allocate(const std::string& baseUrl, const std::string& conferenceId, bool controlling)
{
    _baseUrl = baseUrl;
    _conferenceId = conferenceId;
    using namespace nlohmann;
    json body = {{"action", "allocate"},
        {"bundle-transport",
            {
                {"ice-controlling", controlling},
            }}};

    logger::debug("allocate barbell with %s", "BarbellReq", body.dump().c_str());

    nlohmann::json responseBody;
    auto success = awaitResponse<HttpPostRequest>(_httpd,
        baseUrl + "/barbell/" + conferenceId + "/" + _id,
        body.dump(),
        9 * utils::Time::sec,
        responseBody);

    if (success)
    {
        _offer = responseBody;
        logger::debug("barbell allocated:%s", "BarbellReq", _offer.dump().c_str());
    }
    else
    {
        logger::error("failed to allocate barbell", "BarbellReq");
    }

    return _offer.dump();
}

void Barbell::configure(const std::string& body)
{
    auto requestBody = nlohmann::json::parse(body);
    requestBody["action"] = "configure";

    nlohmann::json responseBody;
    auto success = awaitResponse<HttpPostRequest>(_httpd,
        _baseUrl + "/barbell/" + _conferenceId + "/" + _id,
        requestBody.dump(),
        9 * utils::Time::sec,
        responseBody);

    if (success)
    {
        _offer = responseBody;
    }
    else
    {
        logger::error("failed to configure barbell", "BarbellReq");
    }
}

void Barbell::remove(const std::string& baseUrl)
{
    nlohmann::json responseBody;
    auto success = awaitResponse<HttpDeleteRequest>(_httpd,
        _baseUrl + "/barbell/" + _conferenceId + "/" + _id,
        9 * utils::Time::sec,
        responseBody);

    if (!success)
    {
        logger::error("Failed to delete barbell", "BarbellReq");
    }
}

} // namespace emulator
