#pragma once
#include "test/bwe/FakeVideoSource.h"
#include <algorithm>
#include <list>
#include <vector>

#define DEBUG_FAKE_VIDEO_DECODER 0

#if DEBUG_FAKE_VIDEO_DECODER
#define FAKE_VIDEO_DECODER_LOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
#define FAKE_VIDEO_DECODER_LOG(fmt, ...)
#endif

namespace emulator
{
class FakeVideoDecoder
{
public:
    struct AssembledFrame
    {
        uint32_t ssrc;
        uint32_t frameNum;
        size_t endpointIdHash;
        uint32_t tag;
        bool keyFrame;
    };

    struct Stats
    {
        struct FrameSequence
        {
            size_t endpointHashId;
            uint32_t numFrames;
            uint16_t tag;
        };
        Stats()
            : averageFrameRateDelta(0),
              maxFrameRateDelta(0),
              numDecodedFrames(0),
              lastDecodedFrameNum(0),
              numReceivedPackets(0),
              maxReorderFrameCount(0),
              maxReorderPacketCount(0)
        {
        }
        uint64_t averageFrameRateDelta;
        uint64_t maxFrameRateDelta;
        uint32_t numDecodedFrames;
        uint32_t lastDecodedFrameNum;
        uint32_t numReceivedPackets;
        size_t maxReorderFrameCount;
        size_t maxReorderPacketCount;
        std::list<FrameSequence> frameSequences;
    };

public:
    FakeVideoDecoder(const size_t endpointIdHash, const size_t instanceId);
    void process(const uint8_t* packet, uint32_t length, const uint64_t timestamp);
    void resetPacketCache();
    Stats getStats() const { return _stats; }

private:
    void decodeAssembledFrames(const uint64_t timestamp);
    void removeDecodedPackets(uint32_t frameNum);
    bool assembleFrame(const fakenet::FakeVideoFrameData&);
    void updateStats(const size_t endpointHashId, const uint16_t tag, size_t reoderQueueSize, const uint64_t timestamp);

private:
    uint32_t _lastDecodedFrameNum;
    uint32_t _ssrc;
    uint64_t _lastDecodedFrameTs;
    Stats _stats;
    std::list<fakenet::FakeVideoFrameData> _packets;
    std::list<AssembledFrame> _assembledFrames;
    logger::LoggableId _loggableId;
};
} // namespace emulator