#include "bridge/engine/OpusDecodeJob.h"
#include "bridge/engine/EngineMixer.h"
#include "codec/Opus.h"
#include "codec/OpusDecoder.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtpHeader.h"
#include "transport/RtcTransport.h"
#include "utils/CheckedCast.h"

namespace bridge
{

OpusDecodeJob::OpusDecodeJob(memory::Packet* packet,
    memory::PacketPoolAllocator& allocator,
    transport::RtcTransport* sender,
    bridge::EngineMixer& engineMixer,
    bridge::SsrcInboundContext& ssrcContext)
    : CountedJob(sender->getJobCounter()),
      _packet(packet),
      _allocator(allocator),
      _engineMixer(engineMixer),
      _sender(sender),
      _ssrcContext(ssrcContext)
{
    assert(packet);
    assert(packet->getLength() > 0);
}

OpusDecodeJob::~OpusDecodeJob()
{
    if (_packet)
    {
        _allocator.free(_packet);
        _packet = nullptr;
    }
}

void OpusDecodeJob::run()
{
    const auto rtpHeader = reinterpret_cast<rtp::RtpHeader*>(_packet->get());
    const uint16_t sequenceNumber = rtpHeader->sequenceNumber;

    if (!_ssrcContext._opusDecoder)
    {
        logger::debug("Creating new opus decoder for ssrc %u in mixer %s",
            "OpusDecodeJob",
            _ssrcContext._ssrc,
            _engineMixer.getLoggableId().c_str());
        _ssrcContext._opusDecoder.reset(new codec::OpusDecoder());
    }

    codec::OpusDecoder& decoder = *_ssrcContext._opusDecoder;

    if (!decoder.isInitialized())
    {
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    uint8_t decodedData[memory::Packet::size];
    auto rtpPacket = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpPacket)
    {
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    const uint32_t headerLength = rtpPacket->headerLength();
    const uint32_t payloadLength = _packet->getLength() - headerLength;
    auto payloadStart = rtpPacket->getPayload();

    if (decoder.getSequenceNumber() != 0)
    {
        const auto expectedSequenceNumber = decoder.getSequenceNumber() + 1;

        if (sequenceNumber <= decoder.getSequenceNumber()) // TODO, this can wrap more or less immediately
        {
            logger::debug("Old opus packet sequence %u expected %u, discarding",
                "OpusDecodeJob",
                sequenceNumber,
                expectedSequenceNumber);
            _allocator.free(_packet);
            _packet = nullptr;
            return;
        }

        if (sequenceNumber > expectedSequenceNumber)
        {
            logger::debug("Lost opus packet sequence %u expected %u, fec",
                "OpusDecodeJob",
                sequenceNumber,
                expectedSequenceNumber);
            const auto lastPacketDuration = decoder.getLastPacketDuration();

            for (auto i = expectedSequenceNumber; i < sequenceNumber - 2; ++i)
            {
                const auto decodedFrames = decoder.decode(nullptr,
                    payloadLength,
                    decodedData,
                    utils::checkedCast<size_t>(lastPacketDuration),
                    true);
                onPacketDecoded(decodedFrames, decodedData, payloadStart, headerLength);
            }

            const auto decodedFrames = decoder.decode(payloadStart,
                payloadLength,
                decodedData,
                utils::checkedCast<size_t>(lastPacketDuration),
                true);
            onPacketDecoded(decodedFrames, decodedData, payloadStart, headerLength);
        }
    }

    const auto framesInPacketBuffer =
        memory::Packet::size / codec::Opus::channelsPerFrame / codec::Opus::bytesPerSample;

    const auto decodedFrames = decoder.decode(payloadStart, payloadLength, decodedData, framesInPacketBuffer, false);
    onPacketDecoded(decodedFrames, decodedData, payloadStart, headerLength);
    _packet = nullptr;
}

void OpusDecodeJob::onPacketDecoded(const int32_t decodedFrames,
    const uint8_t* decodedData,
    uint8_t* payloadStart,
    const size_t headerLength)
{
    if (decodedFrames > 0)
    {
        const auto decodedPayloadLength = decodedFrames * codec::Opus::channelsPerFrame * codec::Opus::bytesPerSample;
        memcpy(payloadStart, decodedData, decodedPayloadLength);
        _packet->setLength(headerLength + decodedPayloadLength);

        _engineMixer.onMixerAudioRtpPacketDecoded(_sender, _packet, _allocator);
        return;
    }

    logger::error("Unable to decode opus packet, error code %d", "OpusDecodeJob", decodedFrames);
    _allocator.free(_packet);
    _packet = nullptr;
}

} // namespace bridge
