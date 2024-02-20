#include "ApiChannel.h"
#include "HttpRequests.h"
#include "api/JsonUtils.h"
#include "api/utils.h"
#include "test/integration/emulator/Httpd.h"
#include "utils/Base64.h"
#include "utils/Format.h"
#include "utils/IdGenerator.h"
#include "utils/StringBuilder.h"

namespace
{

} // namespace

namespace emulator
{

bool Channel::create(const bool initiator, const CallConfig& config)
{
    _callConfig = config;

    using namespace nlohmann;
    json body = {{"action", "allocate"}};
    json transport = {{"ice-controlling", true},
        {"ice", true},
        {"rtcp-mux", true},
        {"dtls", _callConfig.dtls},
        {"sdes", _callConfig.sdes != srtp::Profile::NULL_CIPHER}};

    if (_callConfig.transportMode == TransportMode::BundledIce)
    {
        body["bundle-transport"] = transport;
    }
    else if (_callConfig.transportMode == TransportMode::StreamTransportIce)
    {
        transport["ice"] = true;
    }
    else if (_callConfig.transportMode == TransportMode::StreamTransportNoIce)
    {
        transport["ice"] = false;
    }

    if (_callConfig.audio)
    {
        body["audio"] = {{"relay-type", _callConfig.relayType.c_str()}};
        if (_callConfig.transportMode != TransportMode::BundledIce)
        {
            body["audio"]["transport"] = transport;
        }
    }
    if (_callConfig.video)
    {
        const std::string relayMode = (_callConfig.relayType.compare("forwarded") == 0 ? "forwarded" : "ssrc-rewrite");
        body["video"] = {{"relay-type", relayMode}};
        if (_callConfig.transportMode != TransportMode::BundledIce)
        {
            body["video"]["transport"] = transport;
        }
    }
    if (_callConfig.transportMode == TransportMode::BundledIce && _callConfig.dtls)
    {
        body["data"] = json::object();
    }
    if (_callConfig.idleTimeout)
    {
        body["idleTimeout"] = _callConfig.idleTimeout;
    }

    logger::debug("allocate ch with %s", "ApiChannel", body.dump().c_str());
    nlohmann::json responseBody;
    auto success = awaitResponse<HttpPostRequest>(_httpd,
        std::string(_callConfig.baseUrl) + "/conferences/" + _callConfig.conferenceId + "/" + _id,
        body.dump(),
        9 * utils::Time::sec,
        responseBody);

    if (success)
    {
        _offer = responseBody;
        logger::debug("allocate offer received %s", "ApiChannel", responseBody.dump().c_str());
        return true;
    }
    else
    {
        logger::error("failed to allocate channel", "ApiChannel");
        return false;
    }
}

nlohmann::json Channel::buildTransportContent(transport::RtcTransport& transport, const std::string& fingerprint)
{
    using namespace nlohmann;
    json transportBody = {{"rtcp-mux", true}};

    if (_callConfig.transportMode == TransportMode::BundledIce ||
        _callConfig.transportMode == TransportMode::StreamTransportIce)
    {
        auto iceCredentials = transport.getLocalIceCredentials();
        transportBody["ice"] = json::object(
            {{"ufrag", iceCredentials.first}, {"pwd", iceCredentials.second}, {"candidates", json::array()}});

        logger::info("local ICE for %s is %s, %s",
            "channel",
            transport.getLoggableId().c_str(),
            iceCredentials.first.c_str(),
            iceCredentials.second.c_str());
        auto candidates = transport.getLocalCandidates();
        for (auto& c : candidates)
        {
            if (!_callConfig.enableIpv6 && c.address.getFamily() == AF_INET6)
            {
                continue;
            }

            auto jsonCandidate = json::object({{"foundation", c.getFoundation()},
                {"component", c.component},
                {"protocol", c.transportType == ice::TransportType::UDP ? "udp" : "tcp"},
                {"priority", c.priority},
                {"ip", c.address.ipToString()},
                {"port", c.address.getPort()},
                {"type", "host"},
                {"generation", 0},
                {"network", 1}});

            transportBody["ice"]["candidates"].push_back(jsonCandidate);
        }
    }
    else
    {
        auto rtpPort = transport.getLocalRtpPort();
        transportBody["connection"] = json::object({{"port", rtpPort.getPort()}, {"ip", rtpPort.ipToString()}});
    }

    if (_callConfig.dtls)
    {
        transportBody["dtls"] = json::object({{"setup", "active"}, {"type", "sha-256"}, {"hash", fingerprint}});
    }
    else if (_callConfig.sdes != srtp::Profile::NULL_CIPHER)
    {
        std::vector<srtp::AesKey> keys;
        transport.getSdesKeys(keys);
        for (auto key : keys)
        {
            if (key.profile == _callConfig.sdes)
            {
                transportBody["sdes"] = json::object({{"profile", api::utils::toString(key.profile)},
                    {"key", utils::Base64::encode(key.keySalt, key.getLength())}});
                break;
            }
        }
    }
    return transportBody;
}

nlohmann::json Channel::buildAudioContent(uint32_t audioSsrc) const
{
    using namespace nlohmann;
    assert(audioSsrc != 0);
    auto payloadTypes = json::array();
    payloadTypes.push_back(json::object({{"id", 111},
        {"name", "opus"},
        {"clockrate", 48000},
        {"channels", 2},
        {"parameters", json::object()},
        {"rtcp-fbs", json::array()}}));

    json body =
        json::object({{"rtp-hdrexts",
                          json::array({{{"id", 1}, {"uri", "urn:ietf:params:rtp-hdrext:ssrc-audio-level"}},
                              {{"id", 3}, {"uri", "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"}},
                              {{"id", 8}, {"uri", "c9:params:rtp-hdrext:info"}}})},
            {"ssrcs", json::array({audioSsrc})},
            {"payload-types", payloadTypes}});

    return body;
}

nlohmann::json Channel::buildVideoContent(const uint32_t* videoSsrcs) const
{
    using namespace nlohmann;
    assert(videoSsrcs);

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

    if (_callConfig.rtx)
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
    for (const uint32_t* pSsrc = videoSsrcs; *pSsrc != 0; pSsrc += 2)
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

    return videoContent;
}

nlohmann::json Channel::buildNeighbours() const
{
    using namespace nlohmann;
    if (!_callConfig.neighbours.empty())
    {
        auto neighbours = json::object({{"action", "mute"}});
        auto groups = json::array();
        for (auto& id : _callConfig.neighbours)
        {
            groups.push_back(id);
        }
        neighbours["groups"] = groups;
        return neighbours;
    }
    return json();
}

bool Channel::sendResponse(transport::RtcTransport& bundleTransport,
    const std::string& fingerprint,
    uint32_t audioSsrc,
    uint32_t* videoSsrcs)
{
    auto transportBody = buildTransportContent(bundleTransport, fingerprint);
    using namespace nlohmann;
    json body = {{"action", "configure"}};

    body["bundle-transport"] = transportBody;

    if (audioSsrc != 0)
    {
        body["audio"] = buildAudioContent(audioSsrc);
    }
    if (videoSsrcs)
    {
        body["video"] = buildVideoContent(videoSsrcs);
    }
    if (_callConfig.dtls)
    {
        body["data"] = json::object({{"port", 5000}});
    }

    if (!_callConfig.neighbours.empty())
    {
        body["neighbours"] = buildNeighbours();
    }

    logger::info("patch channel with %s", "ApiChannel", body.dump().c_str());

    nlohmann::json responseBody;
    auto success = awaitResponse<HttpPutRequest>(_httpd,
        _callConfig.baseUrl + "/conferences/" + _callConfig.conferenceId + "/" + _id,
        body.dump(),
        3 * utils::Time::sec,
        responseBody);

    if (success)
    {
        raw = responseBody.dump();
        return true;
    }
    else
    {
        logger::error("failed to patch channel ", "ApiChannel");
        return false;
    }
}

bool Channel::sendResponse(transport::RtcTransport* audioTransport,
    transport::RtcTransport* videoTransport,
    const std::string& fingerprint,
    uint32_t audioSsrc,
    uint32_t* videoSsrcs)
{
    using namespace nlohmann;
    json body = {{"action", "configure"}};

    if (audioSsrc != 0 && audioTransport)
    {
        body["audio"] = buildAudioContent(audioSsrc);
        body["audio"]["transport"] = buildTransportContent(*audioTransport, fingerprint);
    }
    if (videoSsrcs && videoTransport)
    {
        body["video"] = buildVideoContent(videoSsrcs);
        body["video"]["transport"] = buildTransportContent(*videoTransport, fingerprint);
    }

    if (!_callConfig.neighbours.empty())
    {
        body["neighbours"] = buildNeighbours();
    }

    logger::info("patch channel with %s", "ApiChannel", body.dump().c_str());

    nlohmann::json responseBody;
    auto success = awaitResponse<HttpPutRequest>(_httpd,
        _callConfig.baseUrl + "/conferences/" + _callConfig.conferenceId + "/" + _id,
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
    return success;
}

void Channel::disconnect()
{
    nlohmann::json responseBody;
    auto success = awaitResponse<HttpDeleteRequest>(_httpd,
        _callConfig.baseUrl + "/conferences/" + _callConfig.conferenceId + "/" + _id,
        3 * utils::Time::sec,
        responseBody);

    if (!success)
    {
        logger::error("failed to delete channel ", "ApiChannel");
    }
}

void Channel::configureTransport(transport::RtcTransport& transport,
    memory::AudioPacketPoolAllocator& allocator,
    nlohmann::json transportJson)
{
    using namespace nlohmann;
    if (_callConfig.transportMode != TransportMode::StreamTransportNoIce)
    {
        setRemoteIce(transport, transportJson, ICE_GROUP, allocator);
    }
    else
    {
        json connection = transportJson["connection"];
        auto addr =
            transport::SocketAddress::parse(connection["ip"].get<std::string>(), connection["port"].get<uint16_t>());
        transport.setRemotePeer(addr);
    }

    if (_callConfig.dtls && transportJson.find("dtls") != transportJson.end())
    {
        std::string fingerPrint = transportJson["dtls"]["hash"];
        transport.asyncSetRemoteDtlsFingerprint(transportJson["dtls"]["type"], fingerPrint, true);
    }
    else if (_callConfig.sdes != srtp::NULL_CIPHER && transportJson.find("sdes") != transportJson.end())
    {
        for (auto sdesSpec : transportJson["sdes"])
        {
            srtp::AesKey key;
            key.profile = api::utils::stringToSrtpProfile(sdesSpec["profile"]);
            if (_callConfig.sdes != key.profile)
            {
                continue;
            }
            auto keyLen = utils::Base64::decode(sdesSpec["key"], key.keySalt, sizeof(key.keySalt));
            assert(keyLen == key.getLength());
            transport.asyncSetRemoteSdesKey(key);
            break;
        }
    }
    else
    {
        transport.asyncSetRemoteSdesKey(srtp::AesKey());
    }
}

void Channel::configureTransport(transport::RtcTransport& transport, memory::AudioPacketPoolAllocator& allocator)
{
    if (exists(_offer, "bundle-transport"))
    {
        configureTransport(transport, allocator, _offer["bundle-transport"]);
    }
}

void Channel::configureAudioTransport(transport::RtcTransport& transport, memory::AudioPacketPoolAllocator& allocator)
{
    if (exists(_offer, "audio", "transport"))
    {
        configureTransport(transport, allocator, _offer["audio"]["transport"]);
    }
}

void Channel::configureVideoTransport(transport::RtcTransport& transport, memory::AudioPacketPoolAllocator& allocator)
{
    if (exists(_offer, "video", "transport"))
    {
        configureTransport(transport, allocator, _offer["video"]["transport"]);
    }
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

} // namespace emulator
