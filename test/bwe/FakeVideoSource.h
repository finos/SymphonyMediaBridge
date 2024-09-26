#pragma once
#include "FakeMedia.h"
#include "memory/PacketPoolAllocator.h"
#include "utils/Trackers.h"
namespace fakenet
{

struct FakeVideoFrameData
{
    uint32_t ssrc;
    uint32_t frameNum;
    uint32_t packetId;
    bool lastPacketInFrame;
    bool keyFrame;
    size_t endpointIdHash;
    uint16_t tag;
};

class FakeVideoSource : public MediaSource
{
public:
    FakeVideoSource(memory::PacketPoolAllocator& allocator,
        uint32_t kbps,
        uint32_t ssrc,
        const size_t endpointIdHash = 0,
        const uint16_t tag = 0);
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

    void setBandwidth(uint32_t kbps) override;
    uint32_t getBandwidth() const override { return _bandwidthKbps; }

    double getBitRate() const { return _avgRate.get() / 1000; }
    uint32_t getSsrc() const override { return _ssrc; }

    void requestKeyFrame() { _keyFrame.store(true); }
    bool isKeyFrameRequested() { return _keyFrame.load(); }

    uint32_t getPacketsSent() const { return _packetsSent; }

    uint16_t getTag() const { return _tag; }

private:
    void tryFillFramePayload(unsigned char* packet, size_t length, bool lastInFrame, bool keyFrame) const;
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
    std::atomic_bool _keyFrame;
    uint32_t _packetsSent;
    uint32_t _packetsInFrame;
    const size_t _endpointIdHash;
    const uint16_t _tag;
};

} // namespace fakenet
