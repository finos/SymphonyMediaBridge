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
    FakeVideoDecoder(const size_t endpointIdHash, const size_t instanceId)
        : _lastDecodedFrameNum(0),
          _ssrc(0),
          _lastDecodedFrameTs(0),
          _loggableId((std::string("FakeVideoDecoder-") + std::to_string(endpointIdHash)).c_str(), instanceId)
    {
    }

    void process(const uint8_t* packet, uint32_t length, const uint64_t timestamp)
    {
        static constexpr size_t VP8_HEADER_SIZE = 2;
        assert(length >= sizeof(fakenet::FakeVideoFrameData) + VP8_HEADER_SIZE);
        const auto data = reinterpret_cast<const fakenet::FakeVideoFrameData*>(packet + VP8_HEADER_SIZE);

        if (data->ssrc == 0)
        {
            assert(0); // Padding packets should have been dropped already in the RTP Receiver.
            return;
        }

        _stats.numReceivedPackets++;

        FAKE_VIDEO_DECODER_LOG(
            "process packet for frame: %d, from %zu, ### last packet %c, key frame: %c (cache size = %zu)",
            _loggableId.c_str(),
            data->frameNum,
            data->endpointIdHash,
            data->lastPacketInFrame ? 't' : 'f',
            data->keyFrame ? 't' : 'f',
            _packets.size());

        if (assembleFrame(*data))
        {
            removeDecodedPackets(data->frameNum);
        }
        else
        {
            FAKE_VIDEO_DECODER_LOG("putting packet to cache, frame: %d, from %zu, ###",
                _loggableId.c_str(),
                data->frameNum,
                data->endpointIdHash);

            _packets.push_back(*data);
        }

        decodeAssembledFrames(timestamp);
    }

    void resetPacketCache()
    {
        FAKE_VIDEO_DECODER_LOG("Reset packet cache (size = %zu)", _loggableId.c_str(), _packets.size());
        _packets.clear();
    }

    Stats getStats() const { return _stats; }

private:
    void decodeAssembledFrames(const uint64_t timestamp)
    {
        while (!_assembledFrames.empty())
        {
            auto frameNum = _lastDecodedFrameNum + 1;
            auto nextFrame = std::find_if(_assembledFrames.begin(),
                _assembledFrames.end(),
                [frameNum](const AssembledFrame& frame) { return frame.frameNum == frameNum || frame.keyFrame; });

            if (nextFrame != _assembledFrames.end())
            {
                FAKE_VIDEO_DECODER_LOG("decoded frame: %d, from %zu",
                    _loggableId.c_str(),
                    nextFrame->frameNum,
                    nextFrame->endpointIdHash);

                _lastDecodedFrameNum = nextFrame->frameNum;
                _ssrc = nextFrame->ssrc;

                auto ssrc = _ssrc;
                auto reorderQueueSize = std::count_if(_assembledFrames.begin(),
                    _assembledFrames.end(),
                    [frameNum, ssrc](
                        const AssembledFrame& frame) { return (frame.frameNum > frameNum) && (ssrc == frame.ssrc); });

                updateStats(nextFrame->endpointIdHash, nextFrame->tag, reorderQueueSize, timestamp);

                _assembledFrames.remove_if([nextFrame](const AssembledFrame& frame) {
                    return frame.ssrc != nextFrame->ssrc || frame.frameNum <= nextFrame->frameNum;
                });
            }
            else
            {
                break;
            }
        }
    }

    void removeDecodedPackets(uint32_t frameNum)
    {
        auto elementsToErase = std::remove_if(_packets.begin(),
            _packets.end(),
            [frameNum](const fakenet::FakeVideoFrameData& data) { return data.frameNum == frameNum; });
        if (elementsToErase != _packets.end())
        {
            _packets.erase(elementsToErase);
        }
    }

    bool assembleFrame(const fakenet::FakeVideoFrameData& data)
    {
        std::list<fakenet::FakeVideoFrameData> framePackets;
        framePackets.push_back(data);

        std::copy_if(_packets.begin(),
            _packets.end(),
            std::back_inserter(framePackets),
            [&data](const fakenet::FakeVideoFrameData& packet) {
                return packet.frameNum == data.frameNum && packet.ssrc == data.ssrc;
            });

        framePackets.sort([](const fakenet::FakeVideoFrameData& one, const fakenet::FakeVideoFrameData& another) {
            return one.packetId < another.packetId;
        });

        const auto numPackets = framePackets.size();
        const bool frameComplete =
            numPackets && framePackets.rbegin()->lastPacketInFrame && (numPackets == framePackets.rbegin()->packetId);

        if (frameComplete)
        {
            AssembledFrame frame;
            frame.frameNum = data.frameNum;
            frame.ssrc = data.ssrc;
            frame.keyFrame = framePackets.begin()->keyFrame;
            frame.endpointIdHash = data.endpointIdHash;
            frame.tag = data.tag;
            _assembledFrames.push_back(frame);

            if (!data.lastPacketInFrame)
            {
                auto packedId = data.packetId;
                size_t reorderQueueSize = std::count_if(framePackets.begin(),
                    framePackets.end(),
                    [packedId](const fakenet::FakeVideoFrameData& data) { return data.packetId > packedId; });
                _stats.maxReorderPacketCount = std::max(_stats.maxReorderPacketCount, reorderQueueSize);
            }
        }

        return frameComplete;
    }

    void updateStats(const size_t endpointHashId, const uint16_t tag, size_t reoderQueueSize, const uint64_t timestamp)
    {
        if (_stats.numDecodedFrames > 0)
        {
            assert(timestamp >= _lastDecodedFrameTs);
            auto delta = timestamp - _lastDecodedFrameTs;
            _stats.maxFrameRateDelta = std::max(_stats.maxFrameRateDelta, delta);
            _stats.averageFrameRateDelta =
                (_stats.averageFrameRateDelta * (_stats.numDecodedFrames - 1) + delta) / _stats.numDecodedFrames;
        }

        auto backOfList = _stats.frameSequences.rbegin();
        if (_stats.numDecodedFrames == 0 || backOfList->endpointHashId != endpointHashId || backOfList->tag != tag)
        {
            Stats::FrameSequence sequence;
            sequence.endpointHashId = endpointHashId;
            sequence.numFrames = 0;
            sequence.tag = tag;
            _stats.frameSequences.push_back(sequence);
        }

        _lastDecodedFrameTs = timestamp;

        _stats.frameSequences.rbegin()->numFrames++;
        _stats.lastDecodedFrameNum = _lastDecodedFrameNum;
        _stats.numDecodedFrames++;
        _stats.maxReorderFrameCount = std::max(_stats.maxReorderFrameCount, reoderQueueSize);
    }

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