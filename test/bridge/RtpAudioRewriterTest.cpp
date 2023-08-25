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

        _wallClock = 4000;
    }
    void TearDown() override { _ssrcOutboundContext.reset(); }

protected:
    std::unique_ptr<memory::PacketPoolAllocator> _allocator;
    std::unique_ptr<bridge::SsrcOutboundContext> _ssrcOutboundContext;
    uint64_t _wallClock;
};

TEST_F(RtpAudioRewriterTest, rewrite)
{
    memory::Packet packet;
    packet.setLength(210);

    auto rtpHeader = rtp::RtpHeader::create(packet);
    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = 12;
    rtpHeader->timestamp = 5000;

    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, 12, *rtpHeader, _wallClock);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, 12, 5000));

    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = 13;
    rtpHeader->timestamp = 5000 + 960;
    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, 13, *rtpHeader, _wallClock);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, 13, 5000 + 960));
}

TEST_F(RtpAudioRewriterTest, advTimestamp)
{
    memory::Packet packet;
    packet.setLength(210);

    auto rtpHeader = rtp::RtpHeader::create(packet);
    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = 12;
    rtpHeader->timestamp = 5000;

    const auto seqCount = 12;
    const auto curTimestamp = 5000;

    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, 12, *rtpHeader, _wallClock);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, seqCount, curTimestamp));

    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = 13;
    rtpHeader->timestamp = 5000 + 5 * 960;
    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, 13, *rtpHeader, _wallClock);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, seqCount + 1, curTimestamp + 960 * 5));
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

    const auto curTimestamp = 5000;

    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, extSeqNo, *rtpHeader, _wallClock);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, extSeqNo, curTimestamp));

    extSeqNo = 3;
    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = extSeqNo;
    rtpHeader->timestamp = 5000 - 2 * 960;
    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, extSeqNo, *rtpHeader, _wallClock);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, 3, curTimestamp - 2 * 960));
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

    const auto seqCount = extSeqNo;
    const auto curTimestamp = 5000;

    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, 12, *rtpHeader, _wallClock);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, seqCount, curTimestamp));

    extSeqNo = 201;
    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = extSeqNo;
    rtpHeader->timestamp = 5000 + 189 * 960;
    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, extSeqNo, *rtpHeader, _wallClock);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, seqCount + 189, curTimestamp + 189 * 960));
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

    const auto seqCount = 12;
    const auto curTimestamp = 5000;

    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, extSeqNo, *rtpHeader, _wallClock);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, extSeqNo, curTimestamp));

    _wallClock += 6 * utils::Time::sec;
    extSeqNo = 312;
    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = extSeqNo;
    rtpHeader->timestamp = 5000 + 300 * 960;
    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, extSeqNo, *rtpHeader, _wallClock);
    ASSERT_TRUE(verifyRtp(*rtpHeader, _ssrcOutboundContext->ssrc, seqCount + 1, curTimestamp + 300 * 960));
}

TEST_F(RtpAudioRewriterTest, ssrcSwitchAfterMute)
{
    memory::Packet packet;
    packet.setLength(210);

    const auto curTimestamp = 5000;

    uint32_t extSeqNo = 12;
    auto rtpHeader = rtp::RtpHeader::create(packet);
    rtpHeader->ssrc = 1001;
    rtpHeader->sequenceNumber = extSeqNo;
    rtpHeader->timestamp = curTimestamp;

    const uint32_t outSsrc = _ssrcOutboundContext->ssrc;
    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, 12, *rtpHeader, _wallClock);
    ASSERT_TRUE(verifyRtp(*rtpHeader, outSsrc, extSeqNo, curTimestamp));

    _wallClock += 6 * utils::Time::sec;
    extSeqNo = 0x123A890;
    rtpHeader->ssrc = 1002;
    rtpHeader->sequenceNumber = 0xA890;
    rtpHeader->timestamp = 1235000;
    bridge::AudioRewriter::rewrite(*_ssrcOutboundContext, extSeqNo, *rtpHeader, _wallClock);
    ASSERT_TRUE(verifyRtp(*rtpHeader, outSsrc, 13, curTimestamp + 300 * 960));
}
