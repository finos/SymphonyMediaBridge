#pragma once
#include "api/RtcDescriptors.h"
#include "api/SimulcastGroup.h"
#include "memory/AudioPacketPoolAllocator.h"
#include "nlohmann/json.hpp"
#include "test/integration/emulator/CallConfigBuilder.h"
#include "test/integration/emulator/Httpd.h"
#include "transport/RtcTransport.h"
#include "utils/Span.h"
#include "utils/StdExtensions.h"
#include <string>
#include <unordered_set>

namespace emulator
{

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

struct SimulcastStream
{
    struct Level
    {
        uint32_t ssrc = 0;
        uint32_t feedbackSsrc = 0;

        bool empty() const { return !ssrc && !feedbackSsrc; }
    };

    Level levels[3];
    bool slides = false;
};

struct AnswerOptions
{
    bool rtxDisabled = false;
    std::vector<std::string> neighbours;
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
        std::vector<srtp::AesKey>& srtpKeys) = 0;

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

class Channel : public BaseChannel
{
public:
    Channel(emulator::HttpdFactory* httpd) : BaseChannel(httpd) {}

    void create(const bool initiator, const CallConfig& config) override;

    void sendResponse(const std::pair<std::string, std::string>& iceCredentials,
        const ice::IceCandidates& candidates,
        const std::string& fingerprint,
        uint32_t audioSsrc,
        uint32_t* videoSsrcs,
        std::vector<srtp::AesKey>& srtpKeys) override;

    void configureTransport(transport::RtcTransport& transport, memory::AudioPacketPoolAllocator& allocator) override;

    bool isAudioOffered() const override { return _offer.find("audio") != _offer.end(); }
    void disconnect() override;

    std::unordered_set<uint32_t> getOfferedVideoSsrcs() const override;
    std::vector<api::SimulcastGroup> getOfferedVideoStreams() const override;
    utils::Optional<uint32_t> getOfferedScreensharingSsrc() const override;
    utils::Optional<uint32_t> getOfferedLocalSsrc() const override;
};

class ColibriChannel : public BaseChannel
{
public:
    ColibriChannel(emulator::HttpdFactory* httpd) : BaseChannel(httpd) {}

    void create(const bool initiator, const CallConfig& config) override;

    void sendResponse(const std::pair<std::string, std::string>& iceCredentials,
        const ice::IceCandidates& candidates,
        const std::string& fingerprint,
        uint32_t audioSsrc,
        uint32_t* videoSsrcs,
        std::vector<srtp::AesKey>& srtpKeys) override;

    void configureTransport(transport::RtcTransport& transport, memory::AudioPacketPoolAllocator& allocator) override;

    bool isAudioOffered() const override;
    void disconnect() override;

    std::unordered_set<uint32_t> getOfferedVideoSsrcs() const override;
    std::vector<api::SimulcastGroup> getOfferedVideoStreams() const override;
    utils::Optional<uint32_t> getOfferedScreensharingSsrc() const override;
    utils::Optional<uint32_t> getOfferedLocalSsrc() const override;
};

class Barbell
{
public:
    Barbell(emulator::HttpdFactory* httpd);

    std::string allocate(const std::string& baseUrl, const std::string& conferenceId, bool controlling);
    void remove(const std::string& baseUrl);
    void configure(const std::string& body);
    const std::string& getId() const { return _id; }

private:
    emulator::HttpdFactory* _httpd;
    std::string _id;
    nlohmann::json _offer;
    std::string _baseUrl;
    std::string _conferenceId;
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
