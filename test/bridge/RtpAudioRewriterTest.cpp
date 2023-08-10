#include "bridge/engine/AudioRewriter.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/Vp8Header.h"
#include "memory/PacketPoolAllocator.h"
#include <array>
#include <gtest/gtest.h>
#include <memory>

namespace
{

const uint32_t outboundSsrc = 12345;

bool verifyRtp(rtp::RtpHeader& header, uint32_t ssrc, uint32_t seqNo, uint32_t timestamp)
{
    EXPECT_EQ(header.ssrc.get(), ssrc);
    EXPECT_EQ(header.timestamp.get(), timestamp);
    EXPECT_EQ(header.sequenceNumber.get(), seqNo & 0xFFFFu);

    return header.ssrc.get() == ssrc //
        && header.timestamp.get() == timestamp //
        && header.sequenceNumber.get() == (seqNo & 0xFFFFu);
}
} // namespace

class RtpAudioRewriterTest : public ::testing::Test
{
    void SetUp() override
    {
        _allocator = std::make_unique<memory::PacketPoolAllocator>(16, "RtpAudioRewriterTest");
        _ssrcOutboundContext = std::make_unique<bridge::SsrcOutboundContext>(outboundSsrc,
            *_allocator,
            bridge::RtpMap(bridge::RtpMap::Format::OPUS));
    }
    void TearDown() override { _ssrcOutboundContext.reset(); }

protected:
    std::unique_ptr<memory::PacketPoolAllocator> _allocator;
    std::unique_ptr<bridge::SsrcOutboundContext> _ssrcOutboundContext;
};

TEST_F(RtpAudioRewriterTest, rewrite)
{
    memory::Packet packet;
    packet.setLength(210);

    auto rtpHeader = rtp::RtpHeader::create(packet);
    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = 12;
    rtpHeader->timestamp = 5000;

    const auto seqCount = _ssrcOutboundContext->rewrite.lastSent.sequenceNumber;
    const auto curTimestamp = _ssrcOutboundContext->rewrite.lastSent.timestamp;

    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, 12, *rtpHeader);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, seqCount + 1, curTimestamp + 960));

    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = 13;
    rtpHeader->timestamp = 5000 + 960;
    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, 13, *rtpHeader);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, seqCount + 2, curTimestamp + 960 * 2));
}

TEST_F(RtpAudioRewriterTest, advTimestamp)
{
    memory::Packet packet;
    packet.setLength(210);

    auto rtpHeader = rtp::RtpHeader::create(packet);
    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = 12;
    rtpHeader->timestamp = 5000;

    const auto seqCount = _ssrcOutboundContext->rewrite.lastSent.sequenceNumber;
    const auto curTimestamp = _ssrcOutboundContext->rewrite.lastSent.timestamp;

    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, 12, *rtpHeader);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, seqCount + 1, curTimestamp + 960));

    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = 13;
    rtpHeader->timestamp = 5000 + 2 * 960;
    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, 13, *rtpHeader);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, seqCount + 2, curTimestamp + 960 * 3));
}

TEST_F(RtpAudioRewriterTest, prevSequenceNumber)
{
    memory::Packet packet;
    packet.setLength(210);

    uint32_t extSeqNo = 12;
    auto rtpHeader = rtp::RtpHeader::create(packet);
    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = extSeqNo;
    rtpHeader->timestamp = 5000;

    const auto seqCount = _ssrcOutboundContext->rewrite.lastSent.sequenceNumber;
    const auto curTimestamp = _ssrcOutboundContext->rewrite.lastSent.timestamp;

    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, 12, *rtpHeader);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, seqCount + 1, curTimestamp + 960));

    extSeqNo = 3;
    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = extSeqNo;
    rtpHeader->timestamp = 5000 - 2 * 960;
    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, extSeqNo, *rtpHeader);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, seqCount - 8, curTimestamp - 960));
}

TEST_F(RtpAudioRewriterTest, smallGap)
{
    memory::Packet packet;
    packet.setLength(210);

    uint32_t extSeqNo = 12;
    auto rtpHeader = rtp::RtpHeader::create(packet);
    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = extSeqNo;
    rtpHeader->timestamp = 5000;

    const auto seqCount = _ssrcOutboundContext->rewrite.lastSent.sequenceNumber;
    const auto curTimestamp = _ssrcOutboundContext->rewrite.lastSent.timestamp;

    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, 12, *rtpHeader);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, seqCount + 1, curTimestamp + 960));

    extSeqNo = 201;
    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = extSeqNo;
    rtpHeader->timestamp = 5000 + 190 * 960;
    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, extSeqNo, *rtpHeader);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, seqCount + 190, curTimestamp + 191 * 960));
}

TEST_F(RtpAudioRewriterTest, largeGap)
{
    memory::Packet packet;
    packet.setLength(210);

    uint32_t extSeqNo = 12;
    auto rtpHeader = rtp::RtpHeader::create(packet);
    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = extSeqNo;
    rtpHeader->timestamp = 5000;

    const auto seqCount = _ssrcOutboundContext->rewrite.lastSent.sequenceNumber;
    const auto curTimestamp = _ssrcOutboundContext->rewrite.lastSent.timestamp;

    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, 12, *rtpHeader);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, seqCount + 1, curTimestamp + 960));

    extSeqNo = 301;
    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = extSeqNo;
    rtpHeader->timestamp = 5000 + 190 * 960;
    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, extSeqNo, *rtpHeader);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, seqCount + 2, curTimestamp + 191 * 960));
}
