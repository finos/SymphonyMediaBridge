#include "TelephoneEventForwardReceiveJob.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "utils/CheckedCast.h"

using namespace bridge;

TelephoneEventForwardReceiveJob::TelephoneEventForwardReceiveJob(memory::UniquePacket packet,
    transport::RtcTransport* sender,
    EngineMixer& engineMixer,
    SsrcInboundContext& ssrcContext,
    uint32_t extendedSequenceNumber)
    : RtpForwarderReceiveBaseJob(std::move(packet), sender, engineMixer, ssrcContext, extendedSequenceNumber)
{
}

void TelephoneEventForwardReceiveJob::run()
{
    auto rtpHeader = rtp::RtpHeader::fromPacket(*_packet);
    if (!rtpHeader)
    {
        assert(false); // should have been checked multiple times
        return;
    }

    if (!tryUnprotectRtpPacket("TelephoneEventForwardReceiveJob"))
    {
        return;
    }

    assert(rtpHeader->payloadType == utils::checkedCast<uint16_t>(_ssrcContext.telephoneEventRtpMap.payloadType));
    _engineMixer.onForwarderAudioRtpPacketDecrypted(_ssrcContext, std::move(_packet), _extendedSequenceNumber);
}