#pragma once
#include "nlohmann/json.hpp"
#include "test/integration/emulator/CallConfigBuilder.h"
#include "test/integration/emulator/Httpd.h"
#include "transport/RtcTransport.h"
#include "utils/StdExtensions.h"
#include <string>
#include <unordered_set>

namespace emulator
{
std::string newGuuid();
std::string newIdString();

class Conference
{
public:
    explicit Conference(emulator::HttpdFactory* httpd) : _httpd(httpd), _success(false) {}

    void create(const std::string& baseUrl, bool useGlobalPort = true);
    void createFromExternal(const std::string& conferenceId)
    {
        assert(_success == false);
        _success = true;
        _id = conferenceId;
    }

    const std::string& getId() const { return _id; }

    bool isSuccess() const { return _success; }

private:
    emulator::HttpdFactory* _httpd;
    std::string _id;
    bool _success;
};

class BaseChannel
{
public:
    BaseChannel(emulator::HttpdFactory* httpd);

    virtual void create(const bool initiator, const CallConfig& config) = 0;

    virtual void sendResponse(const std::pair<std::string, std::string>& iceCredentials,
        const ice::IceCandidates& candidates,
        const std::string& fingerprint,
        uint32_t audioSsrc,
        uint32_t* videoSsrcs,
        srtp::AesKey& remoteSdesKey) = 0;

    virtual void configureTransport(transport::RtcTransport& transport,
        memory::AudioPacketPoolAllocator& allocator) = 0;

    virtual bool isAudioOffered() const = 0;
    virtual void disconnect() = 0;

    virtual std::unordered_set<uint32_t> getOfferedVideoSsrcs() const = 0;
    virtual std::vector<api::SimulcastGroup> getOfferedVideoStreams() const = 0;
    virtual utils::Optional<uint32_t> getOfferedScreensharingSsrc() const = 0;
    virtual utils::Optional<uint32_t> getOfferedLocalSsrc() const = 0;

    bool skipIpv6 = false;

    void addIpv6RemoteCandidates(transport::RtcTransport& transport);

public:
    bool isSuccess() const { return !_offer.empty(); }
    bool isVideoEnabled() const { return _callConfig.video; }

    nlohmann::json getOffer() const { return _offer; }
    std::string getEndpointId() const { return _id; }
    uint32_t getEndpointIdHash() const { return utils::hash<std::string>{}(_id); }
    std::string raw;

protected:
    static constexpr const char* ICE_GROUP = "ice";
    static constexpr const char* TRANSPORT_GROUP = "transport";

    void setRemoteIce(transport::RtcTransport& transport,
        nlohmann::json bundle,
        const char* candidatesGroupName,
        memory::AudioPacketPoolAllocator& allocator);

    CallConfig _callConfig;
    emulator::HttpdFactory* _httpd;
    std::string _id;

    std::string _audioId;
    std::string _dataId;
    std::string _videoId;

    nlohmann::json _offer;

    ice::IceCandidates _ipv6RemoteCandidates;
};

template <typename RequestT>
bool awaitResponse(HttpdFactory* httpd,
    const std::string& url,
    const std::string& body,
    const uint64_t timeout,
    nlohmann::json& outBody)
{
    if (httpd)
    {
        auto response = httpd->sendRequest(RequestT::method, url.c_str(), body.c_str());
        outBody = nlohmann::json::parse(response._body);
        return true;
    }
    else
    {
        RequestT request(url.c_str(), body.c_str());
        request.awaitResponse(timeout);

        if (request.isSuccess())
        {
            outBody = request.getJsonBody();
            return true;
        }
        else
        {
            logger::warn("request failed %d", "awaitResponse", request.getCode());
        }
        return false;
    }
}

template <typename RequestT>
bool awaitResponse(HttpdFactory* httpd, const std::string& url, const uint64_t timeout, nlohmann::json& outBody)
{
    if (httpd)
    {
        auto response = httpd->sendRequest(RequestT::method, url.c_str(), "");
        if (!response._body.empty())
        {
            outBody = nlohmann::json::parse(response._body);
        }
        return true;
    }
    else
    {
        RequestT request(url.c_str());
        request.awaitResponse(timeout);

        if (request.isSuccess())
        {
            outBody = request.getJsonBody();
            return true;
        }
        else
        {
            logger::warn("request failed %d", "awaitResponse", request.getCode());
        }
        return false;
    }
}

} // namespace emulator
