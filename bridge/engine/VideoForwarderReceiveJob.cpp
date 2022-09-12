#include "bridge/engine/VideoForwarderReceiveJob.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/SendPliJob.h"
#include "codec/Vp8Header.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtpHeader.h"
#include "transport/RtcTransport.h"
#include "utils/CheckedCast.h"
#include "utils/Time.h"
#include <cstdio>

namespace
{

FILE* videoDumpFile = nullptr;
const uint64_t missingPacketsTrackerIntervalMs = 10;

} // namespace

namespace bridge
{

void dumpPacket(FILE* fh, const memory::Packet& packet, size_t cappedSize)
{
    auto pktSize = static_cast<uint16_t>(packet.getLength());
    uint16_t recordSize = static_cast<uint16_t>(cappedSize);
    fwrite(&recordSize, 2, 1, fh);
    fwrite(&pktSize, 2, 1, fh); // original size
    fwrite(packet.get(), recordSize, 1, fh);
}

VideoForwarderReceiveJob::VideoForwarderReceiveJob(memory::UniquePacket packet,
    memory::PacketPoolAllocator& allocator,
    transport::RtcTransport* sender,
    bridge::EngineMixer& engineMixer,
    bridge::SsrcInboundContext& ssrcContext,
    const uint32_t localVideoSsrc,
    const uint32_t extendedSequenceNumber,
    const uint64_t timestamp)
    : CountedJob(sender->getJobCounter()),
      _packet(std::move(packet)),
      _allocator(allocator),
      _engineMixer(engineMixer),
      _sender(sender),
      _ssrcContext(ssrcContext),
      _localVideoSsrc(localVideoSsrc),
      _extendedSequenceNumber(extendedSequenceNumber),
      _timestamp(timestamp)
{
    assert(_packet);
    assert(_packet->getLength() > 0);
}

void VideoForwarderReceiveJob::run()
{
    const auto oldRolloverCounter = _ssrcContext.lastUnprotectedExtendedSequenceNumber >> 16;
    const auto newRolloverCounter = _extendedSequenceNumber >> 16;
    if (newRolloverCounter > oldRolloverCounter)
    {
        logger::debug("Setting new rollover counter for %s, ssrc %u",
            "VideoForwarderReceiveJob",
            _sender->getLoggableId().c_str(),
            _ssrcContext.ssrc);
        if (!_sender->setSrtpRemoteRolloverCounter(_ssrcContext.ssrc, newRolloverCounter))
        {
            logger::error("Failed to set rollover counter srtp %s, ssrc %u, mixer %s",
                "VideoForwarderReceiveJob",
                _sender->getLoggableId().c_str(),
                _ssrcContext.ssrc,
                _engineMixer.getLoggableId().c_str());
            return;
        }
    }

    if (!_sender->unprotect(*_packet))
    {
        const auto header = rtp::RtpHeader::fromPacket(*_packet);
        logger::error("Failed to unprotect srtp %s, ssrc %u, seq %u, eseq %u, lreseq %u, lueseq %u, ts %u, mixer %s",
            "VideoForwarderReceiveJob",
            _sender->getLoggableId().c_str(),
            _ssrcContext.ssrc,
            header != nullptr ? header->sequenceNumber.get() : 0,
            _extendedSequenceNumber,
            _ssrcContext.lastReceivedExtendedSequenceNumber,
            _ssrcContext.lastUnprotectedExtendedSequenceNumber,
            header != nullptr ? header->timestamp.get() : 0,
            _engineMixer.getLoggableId().c_str());
        return;
    }

    _ssrcContext.lastUnprotectedExtendedSequenceNumber = _extendedSequenceNumber;
    auto rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpHeader)
    {
        logger::debug("%s invalid rtp header. dropping",
            _sender->getLoggableId().c_str(),
            _sender->getLoggableId().c_str());
        return;
    }

    const auto sequenceNumber = rtpHeader->sequenceNumber.get();
    const auto payload = rtpHeader->getPayload();
    const auto payloadSize = _packet->getLength() - rtpHeader->headerLength();

    const auto payloadDescriptorSize = codec::Vp8Header::getPayloadDescriptorSize(payload, payloadSize);
    const bool isKeyframe = codec::Vp8Header::isKeyFrame(payload, payloadDescriptorSize);
    const auto timestampMs = _timestamp / utils::Time::ms;

    ++_ssrcContext.packetsProcessed;
    bool missingPacketsTrackerReset = false;

    if (_ssrcContext.packetsProcessed == 1)
    {
        _ssrcContext.lastReceivedExtendedSequenceNumber = _extendedSequenceNumber;
        _ssrcContext.videoMissingPacketsTracker =
            std::make_shared<VideoMissingPacketsTracker>(missingPacketsTrackerIntervalMs);

        logger::info("Adding missing packet tracker for %s, ssrc %u",
            "VideoForwarderReceiveJob",
            _sender->getLoggableId().c_str(),
            _ssrcContext.ssrc);

        if (isKeyframe)
        {
            logger::info("Received key frame as first packet, %s ssrc %u seq %u, mark %u, pid %u, tid %d, picid %d, "
                         "tl0picidx %d",
                "VideoForwarderReceiveJob",
                _sender->getLoggableId().c_str(),
                _ssrcContext.ssrc,
                sequenceNumber,
                rtpHeader->marker,
                codec::Vp8Header::getPartitionId(payload),
                codec::Vp8Header::getTid(payload),
                codec::Vp8Header::getPicId(payload),
                codec::Vp8Header::getTl0PicIdx(payload));
            _ssrcContext.pliScheduler.onKeyFrameReceived();
        }
        else
        {
            logger::info(
                "First packet was not key frame, %s ssrc %u seq %u, mark %u, pid %u, tid %d, picid %d, tl0picidx %d",
                "VideoForwarderReceiveJob",
                _sender->getLoggableId().c_str(),
                _ssrcContext.ssrc,
                sequenceNumber,
                rtpHeader->marker,
                codec::Vp8Header::getPartitionId(payload),
                codec::Vp8Header::getTid(payload),
                codec::Vp8Header::getPicId(payload),
                codec::Vp8Header::getTl0PicIdx(payload));

            _ssrcContext.pliScheduler.onPliSent(_timestamp);
            _sender->getJobQueue().addJob<SendPliJob>(_localVideoSsrc, _ssrcContext.ssrc, *_sender, _allocator);
        }
    }
    else
    {
        if (isKeyframe)
        {
            logger::info("Received key frame, %s ssrc %u seq %u, mark %u, pid %u, tid %d, picid %d, tl0picidx %d",
                "VideoForwarderReceiveJob",
                _sender->getLoggableId().c_str(),
                _ssrcContext.ssrc,
                sequenceNumber,
                rtpHeader->marker,
                codec::Vp8Header::getPartitionId(payload),
                codec::Vp8Header::getTid(payload),
                codec::Vp8Header::getPicId(payload),
                codec::Vp8Header::getTl0PicIdx(payload));

            _ssrcContext.pliScheduler.onKeyFrameReceived();
            _ssrcContext.videoMissingPacketsTracker->reset(timestampMs);
            missingPacketsTrackerReset = true;
        }
        else
        {
            const auto rttMs = _ssrcContext.sender->getRtt() / utils::Time::ms;
            if (_ssrcContext.pliScheduler.shouldSendPli(_timestamp, rttMs) && _sender->isConnected())
            {
                logger::debug("%s shouldSendPli for inbound ssrc %u, rtt %ums",
                    "VideoForwarderReceiveJob",
                    _sender->getLoggableId().c_str(),
                    _ssrcContext.ssrc,
                    static_cast<uint32_t>(rttMs));

                _ssrcContext.pliScheduler.onPliSent(_timestamp);
                _sender->getJobQueue().addJob<SendPliJob>(_localVideoSsrc, _ssrcContext.ssrc, *_sender, _allocator);
            }
        }
    }

    if (rtpHeader->marker && isKeyframe)
    {
        logger::debug("end of key frame ssrc %u, seqno %u, pid %u, tid %d, picid %d, tl0picidx %d",
            "VideoForwarderReceiveJob",
            _ssrcContext.ssrc,
            rtpHeader->sequenceNumber.get(),
            codec::Vp8Header::getPartitionId(payload),
            codec::Vp8Header::getTid(payload),
            codec::Vp8Header::getPicId(payload),
            codec::Vp8Header::getTl0PicIdx(payload));
    }

    if (videoDumpFile != nullptr)
    {
        dumpPacket(videoDumpFile, *_packet, std::min(size_t(36), _packet->getLength()));
    }

    if (_extendedSequenceNumber <= _ssrcContext.lastReceivedExtendedSequenceNumber)
    {
        logger::info("%s received out of order on %u, seqno %u, last received %u",
            "VideoForwarderReceiveJob",
            _sender->getLoggableId().c_str(),
            _ssrcContext.ssrc,
            _extendedSequenceNumber,
            _ssrcContext.lastReceivedExtendedSequenceNumber);
    }

    assert(_ssrcContext.videoMissingPacketsTracker.get());
    if (_extendedSequenceNumber > _ssrcContext.lastReceivedExtendedSequenceNumber)
    {
        if (_extendedSequenceNumber - _ssrcContext.lastReceivedExtendedSequenceNumber >=
            VideoMissingPacketsTracker::maxMissingPackets)
        {
            logger::info("Resetting full missing packet tracker for %s, ssrc %u",
                "VideoForwarderReceiveJob",
                _sender->getLoggableId().c_str(),
                _ssrcContext.ssrc);
            _ssrcContext.videoMissingPacketsTracker->reset(timestampMs);
        }
        else if (!missingPacketsTrackerReset)
        {
            for (uint32_t missingSequenceNumber = _ssrcContext.lastReceivedExtendedSequenceNumber + 1;
                 missingSequenceNumber != _extendedSequenceNumber;
                 ++missingSequenceNumber)
            {
                _ssrcContext.videoMissingPacketsTracker->onMissingPacket(missingSequenceNumber, timestampMs);
            }
        }

        _ssrcContext.lastReceivedExtendedSequenceNumber = _extendedSequenceNumber;
    }
    else if (_extendedSequenceNumber != _ssrcContext.lastReceivedExtendedSequenceNumber)
    {
        uint32_t extendedSequenceNumber = 0;
        if (!_ssrcContext.videoMissingPacketsTracker->onPacketArrived(sequenceNumber, extendedSequenceNumber) ||
            extendedSequenceNumber != _extendedSequenceNumber)
        {
            logger::debug("%s received unexpected rtp sequence number seq %u ssrc %u, dropping",
                "VideoForwarderReceiveJob",
                _sender->getLoggableId().c_str(),
                sequenceNumber,
                _ssrcContext.ssrc);
            return;
        }
    }

    logger::debug("%s rtp pushed to queue %u, seq %u, extSeq %u",
        "VideoForwarderReceiveJob",
        _sender->getLoggableId().c_str(),
        _ssrcContext.ssrc,
        sequenceNumber,
        _extendedSequenceNumber);

    assert(rtpHeader->payloadType == utils::checkedCast<uint16_t>(_ssrcContext.rtpMap.payloadType));
    _engineMixer.onForwarderVideoRtpPacketDecrypted(_ssrcContext, std::move(_packet), _extendedSequenceNumber);
}

} // namespace bridge
