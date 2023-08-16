#include "bridge/engine/VideoForwarderReceiveJob.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/SendPliJob.h"
#include "codec/H264Header.h"
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
    if (transport::SrtpClient::shouldSetRolloverCounter(_ssrcContext.lastUnprotectedExtendedSequenceNumber,
            _extendedSequenceNumber))
    {
        const uint32_t newRolloverCounter = _extendedSequenceNumber >> 16;
        logger::info("Setting rollover counter for %s, ssrc %u, seqno %u",
            "VideoForwarderReceiveJob",
            _sender->getLoggableId().c_str(),
            _ssrcContext.ssrc,
            _extendedSequenceNumber);
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
        return;
    }

    _ssrcContext.lastUnprotectedExtendedSequenceNumber = _extendedSequenceNumber;
    auto rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpHeader)
    {
        logger::debug("%s invalid rtp header. dropping", "VideoForwarderReceiveJob", _sender->getLoggableId().c_str());
        return;
    }

    const auto sequenceNumber = rtpHeader->sequenceNumber.get();
    const auto payload = rtpHeader->getPayload();
    const auto payloadSize = _packet->getLength() - rtpHeader->headerLength();
    const auto isKeyFrame = _ssrcContext.rtpMap.format == RtpMap::Format::H264
        ? codec::H264Header::isKeyFrame(payload, payloadSize)
        : codec::Vp8Header::isKeyFrame(payload, codec::Vp8Header::getPayloadDescriptorSize(payload, payloadSize));

    ++_ssrcContext.packetsProcessed;
    bool missingPacketsTrackerReset = false;

    if (_ssrcContext.packetsProcessed == 1)
    {
        _ssrcContext.lastReceivedExtendedSequenceNumber = _extendedSequenceNumber;
        _ssrcContext.videoMissingPacketsTracker = std::make_shared<VideoMissingPacketsTracker>();

        logger::info("Adding missing packet tracker for %s, ssrc %u",
            "VideoForwarderReceiveJob",
            _sender->getLoggableId().c_str(),
            _ssrcContext.ssrc);

        if (isKeyFrame)
        {
            logger::info("Received key frame as first packet, %s ssrc %u seq %u, mark %u",
                "VideoForwarderReceiveJob",
                _sender->getLoggableId().c_str(),
                _ssrcContext.ssrc,
                sequenceNumber,
                rtpHeader->marker);
            _ssrcContext.pliScheduler.onKeyFrameReceived();
        }
        else
        {
            logger::info("First packet was not key frame, %s ssrc %u seq %u, mark %u",
                "VideoForwarderReceiveJob",
                _sender->getLoggableId().c_str(),
                _ssrcContext.ssrc,
                sequenceNumber,
                rtpHeader->marker);

            _ssrcContext.pliScheduler.onPliSent(_timestamp);
            _sender->getJobQueue().addJob<SendPliJob>(_localVideoSsrc, _ssrcContext.ssrc, *_sender, _allocator);
        }
    }
    else
    {
        if (isKeyFrame)
        {
            logger::info("Received key frame, %s ssrc %u seq %u, mark %u",
                "VideoForwarderReceiveJob",
                _sender->getLoggableId().c_str(),
                _ssrcContext.ssrc,
                sequenceNumber,
                rtpHeader->marker);

            _ssrcContext.pliScheduler.onKeyFrameReceived();
            _ssrcContext.videoMissingPacketsTracker->reset(_timestamp);
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

    if (rtpHeader->marker && isKeyFrame)
    {
        logger::debug("end of key frame ssrc %u, seqno %u",
            "VideoForwarderReceiveJob",
            _ssrcContext.ssrc,
            rtpHeader->sequenceNumber.get());
    }

    if (videoDumpFile != nullptr)
    {
        dumpPacket(videoDumpFile, *_packet, std::min(size_t(36), _packet->getLength()));
    }

    if (_extendedSequenceNumber <= _ssrcContext.lastReceivedExtendedSequenceNumber &&
        _ssrcContext.packetsProcessed != 1)
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
            _ssrcContext.videoMissingPacketsTracker->reset(_timestamp);
        }
        else if (!missingPacketsTrackerReset)
        {
            for (uint32_t missingSequenceNumber = _ssrcContext.lastReceivedExtendedSequenceNumber + 1;
                 missingSequenceNumber != _extendedSequenceNumber;
                 ++missingSequenceNumber)
            {
                _ssrcContext.videoMissingPacketsTracker->onMissingPacket(missingSequenceNumber, _timestamp);
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

    assert(rtpHeader->payloadType == utils::checkedCast<uint16_t>(_ssrcContext.rtpMap.payloadType));
    _engineMixer.onForwarderVideoRtpPacketDecrypted(_ssrcContext, std::move(_packet), _extendedSequenceNumber);
}

} // namespace bridge
