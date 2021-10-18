#include "bridge/engine/AudioForwarderReceiveJob.h"
#include "bridge/engine/ActiveMediaList.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/OpusDecodeJob.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtpHeader.h"
#include "transport/RtcTransport.h"
#include "utils/CheckedCast.h"

namespace bridge
{

AudioForwarderReceiveJob::AudioForwarderReceiveJob(memory::Packet* packet,
    memory::PacketPoolAllocator& allocator,
    transport::RtcTransport* sender,
    bridge::EngineMixer& engineMixer,
    bridge::SsrcInboundContext& ssrcContext,
    ActiveMediaList& activeMediaList,
    const int32_t silenceThresholdLevel,
    const bool hasMixedAudioStreams,
    const uint32_t extendedSequenceNumber)
    : CountedJob(sender->getJobCounter()),
      _packet(packet),
      _allocator(allocator),
      _engineMixer(engineMixer),
      _sender(sender),
      _ssrcContext(ssrcContext),
      _activeMediaList(activeMediaList),
      _silenceThresholdLevel(silenceThresholdLevel),
      _hasMixedAudioStreams(hasMixedAudioStreams),
      _extendedSequenceNumber(extendedSequenceNumber)
{
    assert(packet);
    assert(packet->getLength() > 0);
}

AudioForwarderReceiveJob::~AudioForwarderReceiveJob()
{
    if (_packet)
    {
        _allocator.free(_packet);
        _packet = nullptr;
    }
}

void AudioForwarderReceiveJob::run()
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpHeader)
    {
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }

    const auto rtpHeaderExtensions = rtpHeader->getExtensionHeader();
    if (rtpHeaderExtensions)
    {
        int32_t audioLevel = -1;

        for (const auto& rtpHeaderExtension : rtpHeaderExtensions->extensions())
        {
            if (rtpHeaderExtension.id != _ssrcContext._audioLevelExtensionId)
            {
                continue;
            }

            audioLevel = rtpHeaderExtension.data[0] & 0x7F;
            break;
        }

        _activeMediaList.onNewAudioLevel(_sender->getEndpointIdHash(), audioLevel);

        if (audioLevel >= _silenceThresholdLevel)
        {
            _allocator.free(_packet);
            _packet = nullptr;
            _ssrcContext._markNextPacket = true;
            return;
        }
    }

    const auto oldRolloverCounter = _ssrcContext._lastUnprotectedExtendedSequenceNumber >> 16;
    const auto newRolloverCounter = _extendedSequenceNumber >> 16;
    if (newRolloverCounter > oldRolloverCounter)
    {
        logger::debug("Setting new rollover counter for ssrc %u", "AudioForwarderReceiveJob", _ssrcContext._ssrc);
        if (!_sender->setSrtpRemoteRolloverCounter(_ssrcContext._ssrc, newRolloverCounter))
        {
            logger::error("Failed to set rollover counter srtp %u, mixer %s",
                "AudioForwarderReceiveJob",
                _ssrcContext._ssrc,
                _engineMixer.getLoggableId().c_str());
            _allocator.free(_packet);
            _packet = nullptr;
            return;
        }
    }

    if (!_sender->unprotect(_packet))
    {
        logger::error("Failed to unprotect srtp %u, mixer %s",
            "AudioForwarderReceiveJob",
            _ssrcContext._ssrc,
            _engineMixer.getLoggableId().c_str());
        _allocator.free(_packet);
        _packet = nullptr;
        return;
    }
    _ssrcContext._lastUnprotectedExtendedSequenceNumber = _extendedSequenceNumber;

    if (_hasMixedAudioStreams)
    {
        auto mixerPacket = memory::makePacket(_allocator, *_packet);

        switch (_ssrcContext._rtpMap._format)
        {
        case bridge::RtpMap::Format::OPUS:
            if (!_ssrcContext._serialJobManager
                     .addJob<bridge::OpusDecodeJob>(mixerPacket, _allocator, _sender, _engineMixer, _ssrcContext))
            {
                _allocator.free(mixerPacket);
            }
            break;

        default:
            _allocator.free(mixerPacket);
            break;
        }
    }

    if (_ssrcContext._markNextPacket)
    {
        rtpHeader->marker = 1;
        _ssrcContext._markNextPacket = false;
    }

    assert(rtpHeader->payloadType == utils::checkedCast<uint16_t>(_ssrcContext._rtpMap._payloadType));
    _engineMixer.onForwarderAudioRtpPacketDecrypted(_sender, _packet, _allocator, _extendedSequenceNumber);

    _packet = nullptr;
}

} // namespace bridge
