#pragma once
#include "test/bwe/FakeVideoSource.h"
#include <algorithm>
#include <list>
#include <vector>

namespace emulator
{
class FakeVideoDecoder
{
public:
    FakeVideoDecoder()
        : _numDecodedFrames(0),
          _numReceivedFrames(0),
          _numReceivedPackets(0),
          _lastDecodedFrameNum(0),
          _ssrc(0)
    {
    }
    void process(const uint8_t* packet, uint32_t length, const uint64_t timestamp)
    {
        if (!_numReceivedPackets)
        {
            assert(length >= sizeof(fakenet::FakeVideoFrameData) + 1);
            const auto data = reinterpret_cast<const fakenet::FakeVideoFrameData*>(packet + 1);

            bool keyFrame = data->keyFrame;
            if (data->lastPacketInFrame && assembleFrame(data->frameNum, data->lastPacketInFrame, keyFrame) &&
                decode(keyFrame, data->frameNum))
            {
                _lastDecodedFrameNum = data->frameNum;
                removeDecodedPackets(data->frameNum);
            }
            else
            {
                _packets.push_back(*data);
            }
        }
        _numReceivedPackets++;
    }

private:
    bool decode(bool keyFrame, uint32_t frameNum) { return keyFrame || _lastDecodedFrameNum == frameNum - 1; }
    void removeDecodedPackets(uint32_t frameNum)
    {
        _packets.erase(std::remove_if(_packets.begin(),
            _packets.end(),
            [frameNum](const fakenet::FakeVideoFrameData& packet) { return packet.frameNum == frameNum; }));
    }
    bool assembleFrame(uint32_t frameNum, uint32_t lastPacketNum, bool& keyFrame)
    {
        assert(lastPacketNum > 0);
        return lastPacketNum - 1 ==
            std::count_if(_packets.begin(),
                _packets.end(),
                [&keyFrame, frameNum](const fakenet::FakeVideoFrameData& packet) {
                    if (packet.frameNum == frameNum)
                    {
                        if (packet.keyFrame)
                        {
                            keyFrame = true;
                        }
                        return true;
                    }
                    return false;
                });
    }

private:
    uint32_t _numDecodedFrames;
    uint32_t _numReceivedFrames;
    uint32_t _numReceivedPackets;
    uint32_t _lastDecodedFrameNum;
    uint32_t _ssrc;
    std::list<fakenet::FakeVideoFrameData> _packets;
};
} // namespace emulator