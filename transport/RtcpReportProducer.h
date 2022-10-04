#pragma once

#include "concurrency/MpmcHashmap.h"
#include "config/Config.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/RtpReceiveState.h"
#include "transport/RtpSenderState.h"
#include "utils/Optional.h"
#include "utils/Span.h"

namespace transport
{

/**
 * Abbreviations used from RTP RFC 3550
 * - RR receiver report
 * - SR sender report
 * - RB report block, inside an SR or RR
 * */
class RtcpReportProducer
{
public:
    struct RtcpSender
    {
        virtual ~RtcpSender() = default;
        virtual void sendRtcp(memory::UniquePacket packet, uint64_t timestamp) = 0;
    };

    RtcpReportProducer(const logger::LoggableId& loggableId,
        const config::Config& config,
        const concurrency::MpmcHashmap32<uint32_t, RtpSenderState>& outboundSsrcCounters,
        const concurrency::MpmcHashmap32<uint32_t, RtpReceiveState>& inboundSsrcCounters,
        memory::PacketPoolAllocator& rtcpPacketAllocator,
        RtcpSender& rtcpSender);

    /** @return returns true if REMB was sent */
    bool sendReports(uint64_t timestamp, const utils::Optional<uint64_t>& rembMediaBps);

private:
    struct ReportContext
    {
        utils::Span<uint32_t> senderReportSsrcs;
        utils::Span<uint32_t> receiverReportSsrcs;
        utils::Span<uint32_t> activeSsrcs;
        memory::UniquePacket rtcpPacket;
        memory::Packet rembPacket;
    };

    void buildRemb(ReportContext& reportContext, const uint64_t timestamp, uint32_t senderSsrc, uint64_t mediaBps);
    void fillReportContext(ReportContext& report, uint64_t timestamp);

    bool sendSenderReports(ReportContext& report, uint64_t wallClock, uint64_t timestamp);
    bool sendReceiverReports(ReportContext& report, uint64_t wallClock, uint64_t timestamp, uint32_t receiveReportSsrc);

private:
    const logger::LoggableId& _loggableId;
    const config::Config& _config;
    const concurrency::MpmcHashmap32<uint32_t, RtpSenderState>& _outboundSsrcCounters;
    const concurrency::MpmcHashmap32<uint32_t, RtpReceiveState>& _inboundSsrcCounters;
    memory::PacketPoolAllocator& _rtcpPacketAllocator;
    RtcpSender& _rtcpSender;
};

} // namespace transport
