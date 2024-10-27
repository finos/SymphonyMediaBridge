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
    : RtpForwarderReceiveBaseJob(std::move(packet), sender, engineMixer, ssrcContext, extendedSequenceNumber),
      _allocator(allocator),
      _localVideoSsrc(localVideoSsrc),
      _timestamp(timestamp)
{
}

void VideoForwarderReceiveJob::run()
{
    if (!tryUnprotectRtpPacket("VideoForwarderReceiveJob"))
    {
        return;
    }

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

    if (!_ssrcContext.videoMissingPacketsTracker)
    {
        _ssrcContext.lastReceivedExtendedSequenceNumber = _extendedSequenceNumber - 1;
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
            _ssrcContext.videoMissingPacketsTracker->reset();
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

    if (videoDumpFile != nullptr)
    {
        dumpPacket(videoDumpFile, *_packet, std::min(size_t(36), _packet->getLength()));
    }

    assert(_ssrcContext.videoMissingPacketsTracker.get());
    const auto sequenceAdvance =
        static_cast<int32_t>(_extendedSequenceNumber - _ssrcContext.lastReceivedExtendedSequenceNumber);

    if (sequenceAdvance == 1)
    {
        _ssrcContext.videoMissingPacketsTracker->onPacketArrived(sequenceNumber);
        _ssrcContext.lastReceivedExtendedSequenceNumber = _extendedSequenceNumber;
    }
    else if (sequenceAdvance <= 0)
    {
        logger::info("%s received out of order on %u, seqno %u, last received %u",
            "VideoForwarderReceiveJob",
            _sender->getLoggableId().c_str(),
            _ssrcContext.ssrc,
            _extendedSequenceNumber,
            _ssrcContext.lastReceivedExtendedSequenceNumber);
        _ssrcContext.videoMissingPacketsTracker->onPacketArrived(sequenceNumber);
    }
    else // loss
    {
        if (_extendedSequenceNumber - _ssrcContext.lastReceivedExtendedSequenceNumber >=
            VideoMissingPacketsTracker::maxMissingPackets)
        {
            logger::info("Immense loss. Resetting missing packet tracker for %s, ssrc %u",
                "VideoForwarderReceiveJob",
                _sender->getLoggableId().c_str(),
                _ssrcContext.ssrc);
            _ssrcContext.videoMissingPacketsTracker->reset();
        }
        else
        {
            for (uint32_t missingSequenceNumber = _ssrcContext.lastReceivedExtendedSequenceNumber + 1;
                 missingSequenceNumber != _extendedSequenceNumber;
                 ++missingSequenceNumber)
            {
                if (!_ssrcContext.videoMissingPacketsTracker->onMissingPacket(missingSequenceNumber, _timestamp))
                {
                    logger::info("missing packet tracker is full on %s. ssrc %u",
                        "VideoForwarderReceiveJob",
                        _sender->getLoggableId().c_str(),
                        _ssrcContext.ssrc);
                    break;
                }
            }
        }

        _ssrcContext.lastReceivedExtendedSequenceNumber = _extendedSequenceNumber;
    }

    assert(rtpHeader->payloadType == utils::checkedCast<uint16_t>(_ssrcContext.rtpMap.payloadType));
    _engineMixer.onForwarderVideoRtpPacketDecrypted(_ssrcContext, std::move(_packet), _extendedSequenceNumber);
}

} // namespace bridge
