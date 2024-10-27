#include "RtpForwarderReceiveBaseJob.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "logger/Logger.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/RtcTransport.h"

using namespace bridge;

RtpForwarderReceiveBaseJob::RtpForwarderReceiveBaseJob(memory::UniquePacket&& packet,
    transport::RtcTransport* sender,
    EngineMixer& engineMixer,
    SsrcInboundContext& ssrcContext,
    uint32_t extendedSequenceNumber)
    : CountedJob(sender->getJobCounter()),
      _packet(std::move(packet)),
      _engineMixer(engineMixer),
      _sender(sender),
      _ssrcContext(ssrcContext),
      _extendedSequenceNumber(extendedSequenceNumber)
{
    assert(_packet);
    assert(_packet->getLength() > 0);
}

bool RtpForwarderReceiveBaseJob::tryUnprotectRtpPacket(const char* logGroup)
{
    if (!_ssrcContext.hasDecryptedPackets)
    {
        if (_sender->unprotectFirstRtp(*_packet, _ssrcContext.rocOffset))
        {
            _ssrcContext.lastUnprotectedExtendedSequenceNumber = _extendedSequenceNumber;
            _ssrcContext.hasDecryptedPackets = true;
            return true;
        }
        else
        {
            return false;
        }
    }

    if (transport::SrtpClient::shouldSetRolloverCounter(_ssrcContext.lastUnprotectedExtendedSequenceNumber,
            _extendedSequenceNumber))
    {
        const uint32_t oldRolloverCounter =
            _ssrcContext.rocOffset + (_ssrcContext.lastUnprotectedExtendedSequenceNumber >> 16);
        const uint32_t newRolloverCounter = _ssrcContext.rocOffset + (_extendedSequenceNumber >> 16);

        logger::info("Setting rollover counter for ssrc %u, extseqno %u->%u, seqno %u->%u, roc %u->%u, %s",
            logGroup,
            _ssrcContext.ssrc,
            _ssrcContext.lastUnprotectedExtendedSequenceNumber,
            _extendedSequenceNumber,
            _ssrcContext.lastUnprotectedExtendedSequenceNumber & 0xFFFFu,
            _extendedSequenceNumber & 0xFFFFu,
            oldRolloverCounter,
            newRolloverCounter,
            _sender->getLoggableId().c_str());
        if (!_sender->setSrtpRemoteRolloverCounter(_ssrcContext.ssrc, newRolloverCounter))
        {
            logger::error("Failed to set rollover counter srtp %u, seqno %u->%u, roc %u->%u, %s, %s",
                logGroup,
                _ssrcContext.ssrc,
                _ssrcContext.lastUnprotectedExtendedSequenceNumber & 0xFFFFu,
                _extendedSequenceNumber & 0xFFFFu,
                oldRolloverCounter,
                newRolloverCounter,
                _engineMixer.getLoggableId().c_str(),
                _sender->getLoggableId().c_str());
            return false;
        }
    }

    if (!_sender->unprotect(*_packet))
    {
        return false;
    }
    _ssrcContext.lastUnprotectedExtendedSequenceNumber = _extendedSequenceNumber;
    return true;
}
