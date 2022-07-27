#pragma once
#include "memory/AudioPacketPoolAllocator.h"
#include "nlohmann/json.hpp"
#include "transport/RtcTransport.h"
#include "utils/StdExtensions.h"
#include <string>
#include <unordered_set>

namespace emulator
{
class Conference
{
public:
    void create(const std::string& baseUrl);

    const std::string& getId() const { return _id; }

    bool isSuccess() const { return _success; }

private:
    std::string _id;
    bool _success = false;
};

class BaseChannel
{
public:
    BaseChannel();

    virtual void create(const std::string& baseUrl,
        const std::string& conferenceId,
        const bool initiator,
        const bool audio,
        const bool video,
        const bool forwardMedia) = 0;

    virtual void sendResponse(const std::pair<std::string, std::string>& iceCredentials,
        const ice::IceCandidates& candidates,
        const std::string& fingerprint,
        uint32_t audioSsrc,
        uint32_t* videoSsrcs) = 0;

    virtual void configureTransport(transport::RtcTransport& transport,
        memory::AudioPacketPoolAllocator& allocator) = 0;

    virtual bool isAudioOffered() const = 0;

    virtual std::unordered_set<uint32_t> getOfferedVideoSsrcs() const = 0;

public:
    bool isSuccess() const { return !raw.empty(); }
    bool isVideoEnabled() const { return _videoEnabled; }

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

protected:
    std::string _id;
    std::string _conferenceId;

    std::string _audioId;
    std::string _dataId;
    std::string _videoId;
    std::string _relayType;
    nlohmann::json _offer;
    std::string _baseUrl;
    bool _videoEnabled;
};

class Channel : public BaseChannel
{
public:
    void create(const std::string& baseUrl,
        const std::string& conferenceId,
        const bool initiator,
        const bool audio,
        const bool video,
        const bool forwardMedia) override;

    void sendResponse(const std::pair<std::string, std::string>& iceCredentials,
        const ice::IceCandidates& candidates,
        const std::string& fingerprint,
        uint32_t audioSsrc,
        uint32_t* videoSsrcs) override;

    void configureTransport(transport::RtcTransport& transport, memory::AudioPacketPoolAllocator& allocator) override;

    bool isAudioOffered() const override { return _offer.find("audio") != _offer.end(); }

    std::unordered_set<uint32_t> getOfferedVideoSsrcs() const override;
};

class ColibriChannel : public BaseChannel
{
public:
    void create(const std::string& baseUrl,
        const std::string& conferenceId,
        const bool initiator,
        const bool audio,
        const bool video,
        const bool forwardMedia) override;

    void sendResponse(const std::pair<std::string, std::string>& iceCredentials,
        const ice::IceCandidates& candidates,
        const std::string& fingerprint,
        uint32_t audioSsrc,
        uint32_t* videoSsrcs) override;

    void configureTransport(transport::RtcTransport& transport, memory::AudioPacketPoolAllocator& allocator) override;

    bool isAudioOffered() const override;

    std::unordered_set<uint32_t> getOfferedVideoSsrcs() const override;
};

class Barbell
{
public:
    Barbell();

    std::string allocate(const std::string& baseUrl, const std::string& conferenceId, bool controlling);

    void configure(const std::string& body);
    const std::string& getId() const { return _id; }

private:
    std::string _id;
    nlohmann::json _offer;
    std::string _baseUrl;
    std::string _conferenceId;
};

} // namespace emulator
