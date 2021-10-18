#pragma once

#include "jobmanager/Job.h"
#include "utils/Optional.h"
#include <cstdint>

namespace transport
{
class RtcTransport;
} // namespace transport

namespace bridge
{

class SsrcOutboundContext;

class SendRtpPaddingJob : public jobmanager::CountedJob
{
public:
    SendRtpPaddingJob(SsrcOutboundContext& ssrcOutboundContext,
        transport::RtcTransport& transport,
        const uint32_t ssrc,
        const uint32_t numPackets,
        const uint32_t intervalMs,
        const utils::Optional<uint8_t>& absSendTimeExtensionId,
        const uint64_t rtpTimestamp,
        const uint32_t iterationsLeft,
        const uint32_t firstSequenceNumber,
        const uint32_t targetBitrateKbps);

    void run() override;

private:
    SsrcOutboundContext& _ssrcOutboundContext;
    transport::RtcTransport& _transport;
    uint32_t _ssrc;
    uint32_t _numPackets;
    uint32_t _intervalMs;
    utils::Optional<uint8_t> _absSendTimeExtensionId;
    uint64_t _rtpTimestamp;
    uint32_t _iterationsLeft;
    uint32_t _firstSequenceNumber;
    uint32_t _targetBitrateKbps;
};

} // namespace bridge
