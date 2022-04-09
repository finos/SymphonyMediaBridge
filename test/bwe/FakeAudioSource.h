#pragma once
#include "FakeMedia.h"
#include "memory/PacketPoolAllocator.h"
#include "utils/Time.h"
namespace fakenet
{
class FakeAudioSource : public MediaSource
{
public:
    FakeAudioSource(memory::PacketPoolAllocator& allocator, uint32_t bandwidthKbsp, uint32_t ssrc);
    ~FakeAudioSource() {}

    memory::UniquePacket getPacket(uint64_t timestamp) override;
    int64_t timeToRelease(uint64_t timestamp) const override
    {
        if (_bandwidthKbps == 0 || _counter == 0)
        {
            return utils::Time::ms * 40;
        }
        return std::max(int64_t(0), static_cast<int64_t>(_releaseTime - timestamp));
    }

    void setBandwidth(uint32_t kbps) override
    {
        _bandwidthKbps = kbps;
        _counter = 0;
    }
    uint32_t getBandwidth() const override { return _bandwidthKbps; }

    uint8_t getPayloadType() const { return _payloadType; }

    uint32_t getSsrc() const override { return _ssrc; }

private:
    size_t randomPacketSize();

    memory::PacketPoolAllocator& _allocator;
    uint64_t _releaseTime;
    uint32_t _bandwidthKbps;
    uint32_t _counter;
    uint32_t _talkSprint;
    uint32_t _sequenceCounter;
    const uint32_t _ssrc;
    uint32_t _rtpTimestamp;
    uint8_t _payloadType;
};

} // namespace fakenet
