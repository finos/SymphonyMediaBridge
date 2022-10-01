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

class RtcpReportProducer
{
public:
    struct RtcpSender
    {
        virtual ~RtcpSender() = default;
        virtual void sendRtcp(memory::UniquePacket&& packet, uint64_t timestamp) = 0;
    };

    RtcpReportProducer(const logger::LoggableId& loggableId,
        const config::Config& config,
        const concurrency::MpmcHashmap32<uint32_t, RtpSenderState>& outboundSsrcCounters,
        const concurrency::MpmcHashmap32<uint32_t, RtpReceiveState>& inboundSsrcCounters,
        memory::PacketPoolAllocator& rtcpPacketAllocator,
        RtcpSender& rtcpSender);

    /** @return returns true if REMB was sent; otherwise, false */
    bool sendReports(uint64_t timestamp, const utils::Optional<uint64_t>& rembMediaBps);

private:
    struct ReportContext
    {
        utils::Span<uint32_t> senderReportSsrcs;
        utils::Span<uint32_t> receiverReportSsrcs;
        utils::Span<uint32_t> activeSsrcs;
        memory::UniquePacket rtcpPacket;
        memory::Packet rembPacket;
        uint32_t senderReportCount = 0;
        uint32_t receiverReportCount = 0;
        uint32_t activeCount = 0;
    };

    void buildRemb(ReportContext& reportContext, const uint64_t timestamp, uint32_t senderSsrc, uint64_t mediaBps);
    void fillReportContext(ReportContext& report, uint64_t timestamp);

    bool sendSenderReports(ReportContext& report, uint64_t wallClock, int64_t timestamp);
    bool sendReceiveReports(ReportContext& report, uint64_t wallClock, int64_t timestamp, uint32_t receiveReportSsrc);

private:
    const logger::LoggableId& _loggableId;
    const config::Config& _config;
    const concurrency::MpmcHashmap32<uint32_t, RtpSenderState>& _outboundSsrcCounters;
    const concurrency::MpmcHashmap32<uint32_t, RtpReceiveState>& _inboundSsrcCounters;
    memory::PacketPoolAllocator& _rtcpPacketAllocator;
    RtcpSender& _rtcpSender;
};

} // namespace transport