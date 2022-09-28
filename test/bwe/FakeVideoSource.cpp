#include "FakeVideoSource.h"
#include "FakeMedia.h"
#include "logger/Logger.h"
#include "rtp/RtpHeader.h"

namespace fakenet
{

FakeVideoSource::FakeVideoSource(memory::PacketPoolAllocator& allocator, uint32_t kbps, uint32_t ssrc)
    : _allocator(allocator),
      _releaseTime(0),
      _frameReleaseTime(0),
      _bandwidthKbps(kbps),
      _counter(0),
      _frameSize(0),
      _fps(30),
      _pacing(0),
      _mtu(1400),
      _ssrc(ssrc),
      _sequenceCounter(0),
      _avgRate(0.0005),
      _rtpTimestamp(5000),
      _keyFrame(true),
      _packetsSent(0),
      _packetsInFrame(0)
{
    logger::info("created fake video source %u", "FakeVideoSource", ssrc);
}

void FakeVideoSource::tryFillFramePayload(unsigned char* packet, size_t length, bool lastInFrame, bool keyFrame) const
{
    auto rtpHeader = rtp::RtpHeader::fromPtr(packet, length);
    if (rtpHeader->headerLength() + 1 < length)
    {
        auto payload = rtpHeader->getPayload();
        if (keyFrame)
        {
            payload[0] = 1 << 4; // Partition ID: 0, payloadDescriptorSize: 1
        }
        else
        {
            payload[0] = 0;
        }
        payload[1] = 0; // payload[payloadDescriptorSize] & 0x1) == 0x0

        bool hasSpaceForPayload = rtpHeader->headerLength() + 1 + sizeof(FakeVideoFrameData) <= length;
        assert(hasSpaceForPayload);
        if (hasSpaceForPayload)
        {
            FakeVideoFrameData frameData = {_ssrc, _counter, _packetsInFrame, lastInFrame, keyFrame};
            memcpy(payload + 1, &frameData, sizeof(frameData));
        }
    }
}

memory::UniquePacket FakeVideoSource::getPacket(uint64_t timestamp)
{
    if (_bandwidthKbps == 0)
    {
        return nullptr;
    }

    if (_releaseTime == 0 && _counter == 0)
    {
        _releaseTime = timestamp;
        _frameReleaseTime = timestamp;
    }

    auto packetSize = _frameSize;
    if (_frameSize > 2 * _mtu)
    {
        packetSize = _mtu;
    }
    else if (_frameSize > _mtu)
    {
        packetSize = _frameSize / 2;
    }

    if (packetSize > 0 && utils::Time::diff(timestamp, _releaseTime) <= 0)
    {
        auto packet = memory::makeUniquePacket(_allocator);
        if (packet)
        {
            packet->setLength(packetSize);
            auto rtpHeader = rtp::RtpHeader::create(*packet);
            rtpHeader->ssrc = _ssrc;
            rtpHeader->payloadType = 100;
            rtpHeader->sequenceNumber = _sequenceCounter++;
            rtpHeader->timestamp = _rtpTimestamp;

            rtp::RtpHeaderExtension extensionHead;
            rtp::GeneralExtension1Byteheader absSendTime(3, 3);
            auto cursor = extensionHead.extensions().begin();
            extensionHead.addExtension(cursor, absSendTime);
            rtpHeader->setExtensions(extensionHead);

            _frameSize -= packetSize;
            bool lastInFrame = false;
            if (_frameSize > 0)
            {
                _releaseTime += _counter % 2 == 0 ? 0 : _pacing;
            }
            else
            {
                _releaseTime = _frameReleaseTime;
                lastInFrame = true;
            }
            _avgRate.update(packet->getLength() * 8, timestamp);
            _packetsInFrame++;

            tryFillFramePayload(packet->get(), packet->getLength(), lastInFrame, _keyFrame);
            _keyFrame = false;

            ++_packetsSent;
            return packet;
        }
        else
        {
            _releaseTime = timestamp + utils::Time::ms * 10;
            logger::warn("allocator depleted", "FakeVideoSource");
        }
    }
    else if (utils::Time::diff(timestamp, _frameReleaseTime) <= 0)
    {
        setNextFrameSize();
        _rtpTimestamp += 90000 / _fps;
        _releaseTime = timestamp;
        _frameReleaseTime += utils::Time::sec / _fps;
        return getPacket(timestamp);
    }

    return nullptr;
}

void FakeVideoSource::setNextFrameSize()
{
    auto meanSize = _bandwidthKbps * 125 / _fps;
    if (_counter % (_fps * 15) == 0)
    {
        meanSize *= 4;
        _keyFrame = true;
    }
    ++_counter;
    _packetsInFrame = 0;
    _frameSize = randomSize(meanSize, 0.2);
    _pacing = (utils::Time::sec / _fps) * _mtu / (2 * (_frameSize + _mtu));
}
} // namespace fakenet
