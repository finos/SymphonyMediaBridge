

#include "bridge/engine/VideoForwarderRtxReceiveJob.h"
#include "memory/PacketPoolAllocator.h"
#include "mocks/EngineMixerSpy.h"
#include "mocks/RtcTransportMock.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>

using namespace bridge;
using namespace testing;
using namespace test;

namespace
{
const bridge::RtpMap VP8_RTP_MAP(bridge::RtpMap::Format::VP8);
const bridge::RtpMap RTX_RTP_MAP(bridge::RtpMap::Format::RTX);

const uint32_t kRtxSsrc = 100;
const uint32_t kMainSsrc = 200;

;
} // namespace

class VideoForwarderRtxReceiveJobTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _allocator = std::make_unique<memory::PacketPoolAllocator>(16, "VideoForwarderRtxReceiveJobTest");
        _engineMixerSpy = EngineMixerSpy::createDefault();

        _ssrcRtxInboundContext = std::make_unique<bridge::SsrcInboundContext>(kRtxSsrc, RTX_RTP_MAP, nullptr, 0, 0, 0);
        _ssrcMainInboundContext =
            std::make_unique<bridge::SsrcInboundContext>(kMainSsrc, VP8_RTP_MAP, nullptr, 0, 0, 0);

        _ssrcMainInboundContext->videoMissingPacketsTracker = std::make_shared<VideoMissingPacketsTracker>();
    }
    void TearDown() override
    {
        _ssrcMainInboundContext = nullptr;
        _ssrcRtxInboundContext = nullptr;
        _engineMixerSpy = nullptr;
        _allocator = nullptr;
    }

    memory::UniquePacket makeRtxPacket(uint16_t rtxPacketSeq, uint32_t mainPacketSeq)
    {
        auto packet = memory::makeUniquePacket(*_allocator);
        packet->setLength(120);
        auto rtpHeader = rtp::RtpHeader::fromPacket(*packet);
        rtpHeader->ssrc = kRtxSsrc;
        rtpHeader->payloadType = RTX_RTP_MAP.payloadType;
        rtpHeader->sequenceNumber = rtxPacketSeq;
        rtpHeader->padding = 0;

        auto payload = rtpHeader->getPayload();
        reinterpret_cast<nwuint16_t&>(payload[0]) = nwuint16_t(mainPacketSeq);
        return packet;
    }

protected:
    std::unique_ptr<memory::PacketPoolAllocator> _allocator;
    std::unique_ptr<test::EngineMixerSpy> _engineMixerSpy;
    std::unique_ptr<bridge::SsrcInboundContext> _ssrcRtxInboundContext;
    std::unique_ptr<bridge::SsrcInboundContext> _ssrcMainInboundContext;
};

TEST_F(VideoForwarderRtxReceiveJobTest, processRtxPacket)
{
    const uint32_t seqNo = 13;
    const uint32_t rtxSeqNo = 2;
    NiceMock<RtcTransportMock> transportMock;

    _ssrcMainInboundContext->videoMissingPacketsTracker->onMissingPacket(seqNo, 1000);
    auto packet = makeRtxPacket(rtxSeqNo, seqNo);
    auto packetRawPointer = packet.get();
    const size_t originalRtxSize = packet->getLength();

    VideoForwarderRtxReceiveJob job(std::move(packet),
        &transportMock,
        *_engineMixerSpy,
        *_ssrcRtxInboundContext,
        *_ssrcMainInboundContext,
        _ssrcMainInboundContext->ssrc,
        1000);

    EXPECT_CALL(transportMock, unprotect(Ref(*packetRawPointer))).WillOnce(Return(true));

    job.run();

    auto& incomingQueue = _engineMixerSpy->spyIncomingForwarderVideoRtp();
    ASSERT_EQ(1, incomingQueue.size());

    EngineMixerSpy::IncomingPacketInfo incomingPacketInfo;
    ASSERT_EQ(true, incomingQueue.pop(incomingPacketInfo));

    auto rtpHeader = rtp::RtpHeader::fromPacket(*incomingPacketInfo.packet());
    ASSERT_NE(nullptr, rtpHeader);

    ASSERT_EQ(kMainSsrc, rtpHeader->ssrc.get());
    ASSERT_EQ(seqNo, rtpHeader->sequenceNumber);

    // The packet should be 2 byte smaller than RTX
    ASSERT_EQ(originalRtxSize - 2, incomingPacketInfo.packet()->getLength());
}

TEST_F(VideoForwarderRtxReceiveJobTest, shouldDropPaddingPackets)
{
    const uint32_t seqNo = 13;
    const uint32_t rtxSeqNo = 2;
    NiceMock<RtcTransportMock> transportMock;

    _ssrcMainInboundContext->videoMissingPacketsTracker->onMissingPacket(seqNo, 1000);
    auto packet = makeRtxPacket(rtxSeqNo, seqNo);
    auto rtpHeader = rtp::RtpHeader::fromPacket(*packet);
    rtpHeader->padding = 1;

    VideoForwarderRtxReceiveJob job(std::move(packet),
        &transportMock,
        *_engineMixerSpy,
        *_ssrcRtxInboundContext,
        *_ssrcMainInboundContext,
        _ssrcMainInboundContext->ssrc,
        1000);

    // Check we didn't spend cpu to decrypt the packet
    EXPECT_CALL(transportMock, unprotect(_)).Times(0);

    job.run();

    auto& incomingQueue = _engineMixerSpy->spyIncomingForwarderVideoRtp();
    ASSERT_EQ(0, incomingQueue.size());
}

TEST_F(VideoForwarderRtxReceiveJobTest, shouldDropWhenUnprotectFails)
{
    const uint32_t seqNo = 13;
    const uint32_t rtxSeqNo = 2;
    NiceMock<RtcTransportMock> transportMock;

    _ssrcMainInboundContext->videoMissingPacketsTracker->onMissingPacket(seqNo, 1000);
    auto packet = makeRtxPacket(rtxSeqNo, seqNo);
    auto packetRawPointer = packet.get();

    VideoForwarderRtxReceiveJob job(std::move(packet),
        &transportMock,
        *_engineMixerSpy,
        *_ssrcRtxInboundContext,
        *_ssrcMainInboundContext,
        _ssrcMainInboundContext->ssrc,
        1000);

    // simulate failed decryption
    EXPECT_CALL(transportMock, unprotect(Ref(*packetRawPointer))).WillOnce(Return(false));

    job.run();

    auto& incomingQueue = _engineMixerSpy->spyIncomingForwarderVideoRtp();
    ASSERT_EQ(0, incomingQueue.size());
}

TEST_F(VideoForwarderRtxReceiveJobTest, shouldDropWhenPacketIsNotTrackedOnMissingPacketsTracker)
{
    const uint32_t seqNo = 13;
    const uint32_t rtxSeqNo = 2;
    NiceMock<RtcTransportMock> transportMock;

    _ssrcMainInboundContext->videoMissingPacketsTracker->onMissingPacket(14, 1000);
    auto packet = makeRtxPacket(rtxSeqNo, seqNo);

    VideoForwarderRtxReceiveJob job(std::move(packet),
        &transportMock,
        *_engineMixerSpy,
        *_ssrcRtxInboundContext,
        *_ssrcMainInboundContext,
        _ssrcMainInboundContext->ssrc,
        1000);

    EXPECT_CALL(transportMock, unprotect(_)).WillRepeatedly(Return(true));

    job.run();

    auto& incomingQueue = _engineMixerSpy->spyIncomingForwarderVideoRtp();
    ASSERT_EQ(0, incomingQueue.size());
}