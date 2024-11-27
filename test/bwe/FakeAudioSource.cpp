#include "FakeAudioSource.h"
#include "FakeMedia.h"
#include "rtp/RtpHeader.h"
#include "utils/Time.h"
namespace fakenet
{

FakeAudioSource::FakeAudioSource(memory::PacketPoolAllocator& allocator, uint32_t bandwidthKbsp, uint32_t ssrc)
    : _allocator(allocator),
      _releaseTime(0),
      _bandwidthKbps(bandwidthKbsp),
      _counter(0),
      _talkSprint(0),
      _sequenceCounter(0),
      _ssrc(ssrc),
      _rtpTimestamp(3000),
      _payloadType(100)
{
}

memory::UniquePacket FakeAudioSource::getPacket(uint64_t timestamp)
{
    if (_releaseTime == 0 && _counter == 0)
    {
        _releaseTime = timestamp; // + utils::Time::ms * 20;
    }
    if (static_cast<int64_t>(timestamp - _releaseTime) >= 0)
    {
        if (_bandwidthKbps == 0)
        {
            return nullptr;
        }
        _releaseTime += utils::Time::ms * 20;
        auto packet = memory::makeUniquePacket(_allocator);
        if (packet)
        {
            auto rtpHeader = rtp::RtpHeader::create(*packet);
            rtpHeader->payloadType = _payloadType;
            rtpHeader->sequenceNumber = _sequenceCounter++;
            rtpHeader->ssrc = _ssrc;
            rtpHeader->timestamp = _rtpTimestamp;
            _rtpTimestamp += 960;

            rtp::RtpHeaderExtension extensionHead;
            rtp::GeneralExtension1Byteheader absSendTime(3, 3);
            auto cursor = extensionHead.extensions().begin();
            extensionHead.addExtension(cursor, absSendTime);
            rtpHeader->setExtensions(extensionHead);

            packet->setLength(randomPacketSize());
            return packet;
        }
        else
        {
            _releaseTime = timestamp + utils::Time::ms * 10;
            logger::warn("allocator depleted", "FakeAudioSource");
        }
    }
    return memory::UniquePacket();
}

size_t FakeAudioSource::randomPacketSize()
{
    const auto meanSize = _bandwidthKbps * 125 / 50;
    if (_counter % 150 == 0)
    {
        if (rand() % 1000 < 300)
        {
            _talkSprint = randomSize(meanSize, 0.5);
        }
        else
        {
            _talkSprint = randomSize(meanSize / 4, 1.0) + 20;
        }
    }
    ++_counter;
    return _talkSprint + randomSize(_talkSprint / 4, 0.75);
}
} // namespace fakenet
