#pragma once
#include "memory/AudioPacketPoolAllocator.h"
#include "test/integration/emulator/BaseChannel.h"

namespace emulator
{

class ColibriChannel : public BaseChannel
{
public:
    ColibriChannel(emulator::HttpdFactory* httpd) : BaseChannel(httpd) {}

    bool create(const bool initiator, const CallConfig& config) override;

    bool sendResponse(transport::RtcTransport& bundleTransport,
        const std::string& fingerprint,
        uint32_t audioSsrc,
        uint32_t* videoSsrcs) override;

    virtual bool sendResponse(transport::RtcTransport* audioTransport,
        transport::RtcTransport* videoTransport,
        const std::string& fingerprint,
        uint32_t audioSsrc,
        uint32_t* videoSsrcs) override
    {
        assert(false);
        return true;
    }

    void configureTransport(transport::RtcTransport& transport, memory::AudioPacketPoolAllocator& allocator) override;
    void configureAudioTransport(transport::RtcTransport& transport,
        memory::AudioPacketPoolAllocator& allocator) override
    {
        assert(false);
    }

    void configureVideoTransport(transport::RtcTransport& transport,
        memory::AudioPacketPoolAllocator& allocator) override
    {
        assert(false);
    }

    bool isAudioOffered() const override;
    void disconnect() override;

    std::unordered_set<uint32_t> getOfferedVideoSsrcs() const override;
    std::vector<api::SimulcastGroup> getOfferedVideoStreams() const override;
    utils::Optional<uint32_t> getOfferedScreensharingSsrc() const override;
    utils::Optional<uint32_t> getOfferedLocalSsrc() const override;
};

} // namespace emulator
