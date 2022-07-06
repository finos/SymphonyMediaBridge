#pragma once
#include "FakeMedia.h"
#include "memory/PacketPoolAllocator.h"
#include "utils/Trackers.h"
namespace fakenet
{
class FakeVideoSource : public MediaSource
{
public:
    FakeVideoSource(memory::PacketPoolAllocator& allocator, uint32_t kbps, uint32_t ssrc);
    ~FakeVideoSource() {}

    memory::UniquePacket getPacket(uint64_t timestamp) override;
    int64_t timeToRelease(uint64_t timestamp) const override
    {
        if (_bandwidthKbps == 0 || _counter == 0)
        {
            return utils::Time::ms * 30;
        }

        return std::max(int64_t(0), static_cast<int64_t>(_releaseTime - timestamp));
    }

    void setBandwidth(uint32_t kbps) override { _bandwidthKbps = kbps; }
    uint32_t getBandwidth() const override { return _bandwidthKbps; }

    double getBitRate() const { return _avgRate.get() / 1000; }
    uint32_t getSsrc() const override { return _ssrc; }

private:
    void tryFillFramePayload(unsigned char*, size_t, bool) const;
    void setNextFrameSize();

    memory::PacketPoolAllocator& _allocator;
    uint64_t _releaseTime;
    uint64_t _frameReleaseTime;
    uint32_t _bandwidthKbps;
    uint32_t _counter;
    uint32_t _frameSize;
    uint32_t _fps;
    uint64_t _pacing;
    const uint32_t _mtu;
    const uint32_t _ssrc;
    uint32_t _sequenceCounter;
    utils::AvgRateTracker _avgRate;
    uint32_t _rtpTimestamp;
    bool _keyFrame;
};

} // namespace fakenet
