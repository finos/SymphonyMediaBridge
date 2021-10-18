#include "bridge/engine/EncodeJob.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/AudioLevel.h"
#include "codec/Opus.h"
#include "codec/OpusEncoder.h"
#include "memory/Packet.h"
#include "rtp/RtpHeader.h"

namespace bridge
{

EncodeJob::EncodeJob(memory::Packet* packet,
    SsrcOutboundContext& outboundContext,
    transport::Transport& transport,
    const uint64_t rtpTimestamp,
    const int32_t audioLevelExtensionId)
    : jobmanager::CountedJob(transport.getJobCounter()),
      _packet(packet),
      _outboundContext(outboundContext),
      _transport(transport),
      _rtpTimestamp(rtpTimestamp),
      _audioLevelExtensionId(audioLevelExtensionId)

{
    assert(packet);
    assert(packet->getLength() > 0);
}

EncodeJob::~EncodeJob()
{
    if (_packet)
    {
        _outboundContext._allocator.free(_packet);
        _packet = nullptr;
    }
}

void EncodeJob::run()
{
    auto rtpPacket = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpPacket)
    {
        _outboundContext._allocator.free(_packet);
        _packet = nullptr;
        return;
    }
    rtpPacket->ssrc = _outboundContext._ssrc;

    auto& targetFormat = _outboundContext._rtpMap;
    if (targetFormat._format == bridge::RtpMap::Format::OPUS)
    {
        if (_audioLevelExtensionId > 0)
        {
            codec::addAudioLevelRtpExtension(_audioLevelExtensionId, *_packet);
        }

        if (!_outboundContext._opusEncoder)
        {
            _outboundContext._opusEncoder.reset(new codec::OpusEncoder());
        }

        const uint32_t headerLength = rtpPacket->headerLength();
        const uint32_t payloadLength = _packet->getLength() - headerLength;
        const size_t frames = payloadLength / EngineMixer::bytesPerSample / EngineMixer::channelsPerFrame;
        const auto payloadStart = rtpPacket->getPayload();

        const size_t payloadMaxSize = memory::Packet::size - headerLength;
        const size_t payloadMaxFrames = payloadMaxSize / codec::Opus::channelsPerFrame / codec::Opus::bytesPerSample;

        uint8_t encodedData[memory::Packet::size];
        const auto encodedBytes =
            _outboundContext._opusEncoder->encode(payloadStart, frames, encodedData, payloadMaxFrames);

        if (encodedBytes <= 0)
        {
            logger::error("Failed to encode opus, %d", "OpusEncodeJob", encodedBytes);
            _outboundContext._allocator.free(_packet);
            _packet = nullptr;
            return;
        }

        std::memcpy(payloadStart, encodedData, encodedBytes);
        _packet->setLength(headerLength + encodedBytes);
        rtpPacket->ssrc = _outboundContext._ssrc;
        rtpPacket->timestamp = (_rtpTimestamp * 48llu) & 0xFFFFFFFFllu;
        rtpPacket->sequenceNumber = _outboundContext._sequenceCounter++ & 0xFFFFu;
        rtpPacket->payloadType = targetFormat._payloadType;
        doSend(_packet);
    }
    else
    {
        _outboundContext._allocator.free(_packet);
        _packet = nullptr;
        logger::warn("Unknown target format %u", "EncodeJob", static_cast<uint16_t>(targetFormat._format));
    }

    _packet = nullptr;
}

void EncodeJob::doSend(memory::Packet* packet)
{
    _transport.protectAndSend(packet, _outboundContext._allocator);
}

} // namespace bridge