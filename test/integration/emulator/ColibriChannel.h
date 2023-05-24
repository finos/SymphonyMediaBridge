#pragma once
#include "memory/AudioPacketPoolAllocator.h"
#include "test/integration/emulator/BaseChannel.h"

namespace emulator
{

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
        srtp::AesKey& remoteSdesKey) override;

    void configureTransport(transport::RtcTransport& transport, memory::AudioPacketPoolAllocator& allocator) override;

    bool isAudioOffered() const override;
    void disconnect() override;

    std::unordered_set<uint32_t> getOfferedVideoSsrcs() const override;
    std::vector<api::SimulcastGroup> getOfferedVideoStreams() const override;
    utils::Optional<uint32_t> getOfferedScreensharingSsrc() const override;
    utils::Optional<uint32_t> getOfferedLocalSsrc() const override;
};

} // namespace emulator
