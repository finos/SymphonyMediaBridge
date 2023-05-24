#pragma once
#include "memory/AudioPacketPoolAllocator.h"
#include "test/integration/emulator/BaseChannel.h"

namespace emulator
{

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
        const srtp::AesKey& remoteSdesKey) override;

    void configureTransport(transport::RtcTransport& transport, memory::AudioPacketPoolAllocator& allocator) override;

    bool isAudioOffered() const override { return _offer.find("audio") != _offer.end(); }
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

} // namespace emulator
