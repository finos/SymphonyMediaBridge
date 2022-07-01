#include "FakeVideoSource.h"
#include "FakeMedia.h"
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
      _rtpTimestamp(5000)
{
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
            if (_frameSize > 0)
            {
                _releaseTime += _counter % 2 == 0 ? 0 : _pacing;
            }
            else
            {
                _releaseTime = _frameReleaseTime;
            }
            _avgRate.update(packet->getLength() * 8, timestamp);
            return packet;
        }
        else
        {
            _releaseTime = timestamp + utils::Time::ms * 10;
            logger::warn("allocator depleted", "FakeAudioSource");
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
    }
    ++_counter;
    _frameSize = randomSize(meanSize, 0.2);
    _pacing = (utils::Time::sec / _fps) * _mtu / (2 * (_frameSize + _mtu));
}
} // namespace fakenet
