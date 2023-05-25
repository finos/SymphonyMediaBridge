#pragma once
#include "memory/AudioPacketPoolAllocator.h"
#include "test/integration/emulator/BaseChannel.h"

namespace emulator
{

class Channel : public BaseChannel
{
public:
    Channel(emulator::HttpdFactory* httpd) : BaseChannel(httpd) {}

    bool create(const bool initiator, const CallConfig& config) override;

    void sendResponse(transport::RtcTransport& bundleTransport,
        const std::string& fingerprint,
        uint32_t audioSsrc,
        uint32_t* videoSsrcs) override;

    void sendResponse(transport::RtcTransport* audioTransport,
        transport::RtcTransport* videoTransport,
        const std::string& fingerprint,
        uint32_t audioSsrc,
        uint32_t* videoSsrcs) override;

    void configureTransport(transport::RtcTransport& transport, memory::AudioPacketPoolAllocator& allocator) override;
    void configureAudioTransport(transport::RtcTransport& transport,
        memory::AudioPacketPoolAllocator& allocator) override;
    void configureVideoTransport(transport::RtcTransport& transport,
        memory::AudioPacketPoolAllocator& allocator) override;

    bool isAudioOffered() const override { return _offer.find("audio") != _offer.end(); }
    void disconnect() override;

    std::unordered_set<uint32_t> getOfferedVideoSsrcs() const override;
    std::vector<api::SimulcastGroup> getOfferedVideoStreams() const override;
    utils::Optional<uint32_t> getOfferedScreensharingSsrc() const override;
    utils::Optional<uint32_t> getOfferedLocalSsrc() const override;

private:
    void configureTransport(transport::RtcTransport& transport,
        memory::AudioPacketPoolAllocator& allocator,
        nlohmann::json transportJson);

    nlohmann::json buildTransportContent(transport::RtcTransport& transport, const std::string& fingerprint);

    nlohmann::json buildAudioContent(uint32_t audioSsrc) const;
    nlohmann::json buildVideoContent(const uint32_t* videoSsrcs) const;
    nlohmann::json buildNeighbours() const;
};

} // namespace emulator
