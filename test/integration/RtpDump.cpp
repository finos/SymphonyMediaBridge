#include "RtpDump.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtpHeader.h"
#include "utils/Time.h"
#include <inttypes.h>
namespace test
{

VideoSource::VideoSource(memory::PacketPoolAllocator& allocator)
    : _allocator(allocator),
      _fHandle(nullptr),
      _packet(nullptr),
      _timeReference(0),
      _rtpTimestampOffset(0),
      _aborted(false)
{
}

bool VideoSource::open(const char* filename)
{
    _fHandle = fopen(filename, "r");
    if (_fHandle)
    {
        for (int i = 0; i < 5; ++i)
        {
            auto packet = readPacket();
            if (!packet)
            {
                break;
            }
            if (rtp::isRtpPacket(*packet))
            {
                auto* header = rtp::RtpHeader::fromPacket(*packet);
                _rtpMap = bridge::RtpMap(bridge::RtpMap::Format::VP8, header->payloadType, 90000);
                rewind(_fHandle);
                break;
            }
        }
    }
    return _fHandle != nullptr;
}

VideoSource::~VideoSource()
{
    if (_fHandle != nullptr)
    {
        fclose(_fHandle);
    }
}

memory::PacketPtr VideoSource::getNext(uint64_t now)
{
    if (_aborted)
    {
        return memory::PacketPtr();
    }
    if (!_packet)
    {
        for (_packet = readPacket(); _packet && !rtp::isRtpPacket(*_packet);)
        {
            _packet = readPacket();
        }
        if (!_packet)
        {
            _aborted = true;
            return nullptr;
        }
        _timeReference = now;
        auto* rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
        _rtpTimestampOffset = rtpHeader->timestamp;
        _cursor.sequenceNumber = rtpHeader->sequenceNumber;
    }

    auto relativeTime = now - _timeReference;
    auto rtpTime = relativeTime * 90 / utils::Time::ms;
    auto* rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
    if (static_cast<int64_t>(rtpTime - _cursor.timestamp) >= 0)
    {
        memory::PacketPtr packet(std::move(_packet));

        rtpHeader->sequenceNumber = _cursor.sequenceNumber++;
        rtpHeader->timestamp = _cursor.timestamp + _rtpTimestampOffset;
        _packet = readPacket();
        if (!_packet)
        {
            rewind(_fHandle);
            _packet = readPacket();
            if (!_packet)
            {
                _aborted = true;
                return memory::PacketPtr();
            }
        }
        auto* nextRtpHeader = rtp::RtpHeader::fromPacket(*_packet);
        _cursor.timestamp += nextRtpHeader->timestamp - rtpHeader->timestamp;
        return packet;
    }

    return memory::PacketPtr();
}

memory::PacketPtr VideoSource::readPacket()
{
    auto packet = memory::makePacketPtr(_allocator);
    uint16_t size;
    uint16_t originalSize;
    int bytesRead = fread(&size, 2, 1, _fHandle);
    bytesRead += fread(&originalSize, 2, 1, _fHandle);
    if (bytesRead != 2)
    {
        return memory::PacketPtr();
    }
    bytesRead = fread(packet->get(), 1, size, _fHandle);
    if (bytesRead != size)
    {
        return memory::PacketPtr();
    }
    packet->setLength(originalSize);
    if (!rtp::isRtpPacket(*packet))
    {
        return readPacket();
    }
    return packet;
}

} // namespace test
