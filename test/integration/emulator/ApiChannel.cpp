#include "ApiChannel.h"
#include "HttpRequests.h"
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

void Channel::create(const bool initiator, const CallConfig& config)
{
    _callConfig = config;

    using namespace nlohmann;
    json body = {{"action", "allocate"},
        {"bundle-transport", {{"ice-controlling", true}, {"ice", true}, {"rtcp-mux", true}}}};

    body["bundle-transport"]["dtls"] = _callConfig.dtls;
    body["bundle-transport"]["sdes"] = _callConfig.sdes;

    if (_callConfig.audio)
    {
        body["audio"] = {{"relay-type", _callConfig.relayType.c_str()}};
    }
    if (_callConfig.video)
    {
        body["video"] = {{"relay-type", "ssrc-rewrite"}};
    }
    if (_callConfig.audio || _callConfig.video)
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
    uint32_t* videoSsrcs,
    srtp::AesKey& remoteSdesKey)
{
    using namespace nlohmann;
    json body = {{"action", "configure"}};

    auto transportSpec = json::object(
        {{"ice", {{"ufrag", iceCredentials.first}, {"pwd", iceCredentials.second}, {"candidates", json::array()}}}});

    if (_callConfig.dtls)
    {
        transportSpec["dtls"] = json::object({{"setup", "active"}, {"type", "sha-256"}, {"hash", fingerprint}});
    }
    if (_callConfig.sdes)
    {
        transportSpec["sdes"] = json::object({{"profile", api::utils::toString(remoteSdesKey.profile)},
            {"key", utils::Base64::encode(remoteSdesKey.keySalt, remoteSdesKey.getLength())}});
    }

    for (auto& c : candidates)
    {
        if (skipIpv6 && c.address.getFamily() == AF_INET6)
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

    if (!_callConfig.neighbours.empty())
    {
        auto neighbours = json::object();
        auto groups = json::array();
        neighbours["action"] = "mute";
        for (auto& id : _callConfig.neighbours)
        {
            groups.push_back(id);
        }
        neighbours["groups"] = groups;

        body["neighbours"] = neighbours;
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

void Channel::configureTransport(transport::RtcTransport& transport, memory::AudioPacketPoolAllocator& allocator)
{
    auto bundle = _offer["bundle-transport"];
    setRemoteIce(transport, bundle, ICE_GROUP, allocator);

    if (bundle.find("dtls") != bundle.end())
    {
        std::string fingerPrint = bundle["dtls"]["hash"];
        transport.asyncSetRemoteDtlsFingerprint(bundle["dtls"]["type"], fingerPrint, true);
    }
    else if (bundle.find("sdes") != bundle.end())
    {
        for (auto sdesSpec : bundle["sdes"])
        {
            srtp::AesKey key;
            key.profile = api::utils::stringToSrtpProfile(sdesSpec["profile"]);
            auto keyLen = utils::Base64::decode(sdesSpec["key"], key.keySalt, sizeof(key.keySalt));
            assert(keyLen == key.getLength());
            transport.asyncSetRemoteSdesKey(key);
            break;
        }
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
