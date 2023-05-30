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

class BaseChannel
{
public:
    BaseChannel(emulator::HttpdFactory* httpd);

    virtual bool create(const bool initiator, const CallConfig& config) = 0;

    virtual bool sendResponse(transport::RtcTransport& bundleTransport,
        const std::string& fingerprint,
        uint32_t audioSsrc,
        uint32_t* videoSsrcs) = 0;

    virtual bool sendResponse(transport::RtcTransport* audioTransport,
        transport::RtcTransport* videoTransport,
        const std::string& fingerprint,
        uint32_t audioSsrc,
        uint32_t* videoSsrcs) = 0;

    virtual void configureTransport(transport::RtcTransport& transport,
        memory::AudioPacketPoolAllocator& allocator) = 0;
    virtual void configureAudioTransport(transport::RtcTransport& transport,
        memory::AudioPacketPoolAllocator& allocator) = 0;
    virtual void configureVideoTransport(transport::RtcTransport& transport,
        memory::AudioPacketPoolAllocator& allocator) = 0;

    virtual bool isAudioOffered() const = 0;
    virtual void disconnect() = 0;

    virtual std::unordered_set<uint32_t> getOfferedVideoSsrcs() const = 0;
    virtual std::vector<api::SimulcastGroup> getOfferedVideoStreams() const = 0;
    virtual utils::Optional<uint32_t> getOfferedScreensharingSsrc() const = 0;
    virtual utils::Optional<uint32_t> getOfferedLocalSsrc() const = 0;

    void addIpv6RemoteCandidates(transport::RtcTransport& transport);

public:
    bool isSuccess() const { return !_offer.empty(); }

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

} // namespace emulator
