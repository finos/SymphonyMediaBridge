
#include "bridge/engine/SsrcOutboundContext.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "codec/Vp8Header.h"
#include "memory/PacketPoolAllocator.h"
#include "utils/SsrcGenerator.h"
#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace
{

// constexpr int32_t MAX_JUMP_AHEAD = bridge::RtpVideoRewriter::MAX_JUMP_AHEAD;

const bridge::RtpMap DEFAULT_OPUS_RTP_MAP(bridge::RtpMap::Format::OPUS);
const bridge::RtpMap DEFAULT_TELEPHONE_EVENT_RTP_MAP(bridge::RtpMap::Format::TELEPHONE_EVENT);
const bridge::RtpMap DEFAULT_VP8_RTP_MAP(bridge::RtpMap::Format::VP8);
const bridge::RtpMap DEFAULT_H264_RTP_MAP(bridge::RtpMap::Format::H264);

const uint32_t kDefaultOutboundSsrc = 4459832;

const int32_t MAX_VIDEO_SEQ_GAP = 512;

rtp::RtpHeader* initializePacket(memory::Packet& packet, const bridge::SsrcInboundContext& inboundContext)
{
    packet.setLength(210);

    auto rtpHeader = rtp::RtpHeader::create(packet);
    rtpHeader->ssrc = inboundContext.ssrc;

    return rtpHeader;
}

void examineVp8(bridge::SsrcOutboundContext& outboundContext,
    bridge::SsrcInboundContext& inboundContext,
    memory::Packet& packet,
    uint32_t seqNo,
    uint32_t timestamp,
    uint32_t expectedSeqNo,
    uint16_t picId,
    uint16_t picIdx,
    uint16_t expectedPicId,
    uint16_t expectedPicIdx,
    uint64_t wallClock)
{
    auto rtpHeader = rtp::RtpHeader::create(packet);
    auto payload = rtpHeader->getPayload();
    rtpHeader->ssrc = inboundContext.ssrc;
    rtpHeader->sequenceNumber = seqNo & 0xFFFFu;
    rtpHeader->timestamp = timestamp;
    codec::Vp8Header::setPicId(payload, picId);
    codec::Vp8Header::setTl0PicIdx(payload, picIdx);

    uint32_t sequenceNumberAfterRewrite = 0;
    ASSERT_TRUE(outboundContext
                    .rewriteVideo(*rtpHeader, inboundContext, seqNo, "", sequenceNumberAfterRewrite, wallClock, false));
    EXPECT_EQ(outboundContext.ssrc, rtpHeader->ssrc.get());
    EXPECT_EQ(expectedSeqNo & 0xFFFFu, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(expectedSeqNo, sequenceNumberAfterRewrite);
    EXPECT_EQ(expectedPicId, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(expectedPicIdx, codec::Vp8Header::getTl0PicIdx(payload));
}

void examineVp8(bridge::SsrcOutboundContext& outboundContext,
    bridge::SsrcInboundContext& inboundContext,
    memory::Packet& packet,
    uint32_t seqNo,
    uint32_t timestamp,
    uint32_t expectedSeqNo,
    uint16_t picId,
    uint16_t picIdx,
    uint16_t expectedPicId,
    uint16_t expectedPicIdx,
    uint32_t expectedTimestamp,
    uint64_t wallClock)
{
    auto rtpHeader = rtp::RtpHeader::create(packet);
    auto payload = rtpHeader->getPayload();
    rtpHeader->ssrc = inboundContext.ssrc;
    rtpHeader->sequenceNumber = seqNo & 0xFFFFu;
    rtpHeader->timestamp = timestamp;
    codec::Vp8Header::setPicId(payload, picId);
    codec::Vp8Header::setTl0PicIdx(payload, picIdx);

    uint32_t sequenceNumberAfterRewrite = 0;
    ASSERT_TRUE(outboundContext
                    .rewriteVideo(*rtpHeader, inboundContext, seqNo, "", sequenceNumberAfterRewrite, wallClock, false));

    EXPECT_EQ(outboundContext.ssrc, rtpHeader->ssrc.get());
    EXPECT_EQ(expectedSeqNo & 0xFFFFu, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(expectedSeqNo, sequenceNumberAfterRewrite);
    EXPECT_EQ(expectedPicId, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(expectedPicIdx, codec::Vp8Header::getTl0PicIdx(payload));
    EXPECT_EQ(expectedTimestamp, rtpHeader->timestamp.get());
}

void expectVideoPacketDrop(bridge::SsrcOutboundContext& outboundContext,
    bridge::SsrcInboundContext& inboundContext,
    memory::Packet& packet,
    uint32_t seqNo,
    uint32_t timestamp,
    uint16_t picId,
    uint16_t picIdx,
    uint64_t wallClock)
{
    auto rtpHeader = rtp::RtpHeader::create(packet);
    auto payload = rtpHeader->getPayload();
    rtpHeader->ssrc = inboundContext.ssrc;
    rtpHeader->sequenceNumber = seqNo & 0xFFFFu;
    rtpHeader->timestamp = timestamp;
    codec::Vp8Header::setPicId(payload, picId);
    codec::Vp8Header::setTl0PicIdx(payload, picIdx);

    uint32_t sequenceNumberAfterRewrite = 0;
    ASSERT_FALSE(
        outboundContext
            .rewriteVideo(*rtpHeader, inboundContext, seqNo, "", sequenceNumberAfterRewrite, wallClock, false));
}

void examineH264(bridge::SsrcOutboundContext& outboundContext,
    bridge::SsrcInboundContext& inboundContext,
    memory::Packet& packet,
    uint32_t seqNo,
    uint32_t timestamp,
    uint32_t expectedSeqNo,
    uint64_t wallClock)
{
    auto rtpHeader = rtp::RtpHeader::create(packet);
    rtpHeader->ssrc = inboundContext.ssrc;
    rtpHeader->sequenceNumber = seqNo & 0xFFFFu;
    rtpHeader->timestamp = timestamp;

    uint32_t sequenceNumberAfterRewrite = 0;
    ASSERT_TRUE(outboundContext
                    .rewriteVideo(*rtpHeader, inboundContext, seqNo, "", sequenceNumberAfterRewrite, wallClock, false));

    EXPECT_EQ(outboundContext.ssrc, rtpHeader->ssrc.get());
    EXPECT_EQ(expectedSeqNo & 0xFFFFu, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(expectedSeqNo, sequenceNumberAfterRewrite);
}

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

class SsrcOutboundContextTest : public ::testing::Test
{
public:
    void SetUp() override
    {
        _allocator = std::make_unique<memory::PacketPoolAllocator>(16, "SsrcOutboundContextTest");
        _wallClock = 4000;
    }

    void TearDown() override
    {
        ssrcOutboundContexts.clear();
        _allocator = nullptr;
    }

    bridge::SsrcOutboundContext* createOutboundContext(uint32_t ssrc,
        const bridge::RtpMap& rtpMap,
        const bridge::RtpMap& telephoneEventRtpMap)
    {
        ssrcOutboundContexts.push_back(
            std::make_unique<bridge::SsrcOutboundContext>(ssrc, *_allocator, rtpMap, telephoneEventRtpMap));

        return ssrcOutboundContexts.back().get();
    }

    bridge::SsrcOutboundContext* createOutboundContext(uint32_t ssrc, const bridge::RtpMap& rtpMap)
    {
        return createOutboundContext(ssrc, rtpMap, bridge::RtpMap::EMPTY);
    }

    bridge::SsrcOutboundContext* createDefaultOutboundContextForAudio()
    {
        return createOutboundContext(generateSsrc(), DEFAULT_OPUS_RTP_MAP, DEFAULT_TELEPHONE_EVENT_RTP_MAP);
    }

    bridge::SsrcOutboundContext* createDefaultOutboundContextForVideoVp8()
    {
        return createOutboundContext(generateSsrc(), DEFAULT_VP8_RTP_MAP, bridge::RtpMap::EMPTY);
    }

    bridge::SsrcOutboundContext* createDefaultOutboundContextForVideoH264()
    {
        return createOutboundContext(generateSsrc(), DEFAULT_H264_RTP_MAP, bridge::RtpMap::EMPTY);
    }

    bridge::SsrcInboundContext* createInboundContextForAudio()
    {
        _ssrcInboundContext.push_back(std::make_unique<bridge::SsrcInboundContext>(generateSsrc(),
            DEFAULT_OPUS_RTP_MAP,
            DEFAULT_TELEPHONE_EVENT_RTP_MAP,
            nullptr,
            _wallClock));

        return _ssrcInboundContext.back().get();
    }

    bridge::SsrcInboundContext* createInboundContextForVideoVp8()
    {
        _ssrcInboundContext.push_back(std::make_unique<bridge::SsrcInboundContext>(generateSsrc(),
            DEFAULT_VP8_RTP_MAP,
            nullptr,
            _wallClock,
            0,
            0));

        return _ssrcInboundContext.back().get();
    }

    bridge::SsrcInboundContext* createInboundContextForVideoH264()
    {
        _ssrcInboundContext.push_back(std::make_unique<bridge::SsrcInboundContext>(generateSsrc(),
            DEFAULT_H264_RTP_MAP,
            nullptr,
            _wallClock,
            0,
            0));

        return _ssrcInboundContext.back().get();
    }

private:
    uint32_t generateSsrc()
    {
        while (true)
        {
            const uint32_t ssrc = _ssrcGenerator.next();

            // check collisions
            bool collisionDetected = (ssrc == kDefaultOutboundSsrc);
            collisionDetected |= std::find_if(_ssrcInboundContext.begin(),
                                     _ssrcInboundContext.end(),
                                     [&](const auto& c) { return c->ssrc == ssrc; }) != _ssrcInboundContext.end();

            collisionDetected |= std::find_if(ssrcOutboundContexts.begin(),
                                     ssrcOutboundContexts.end(),
                                     [&](const auto& c) { return c->ssrc == ssrc; }) != ssrcOutboundContexts.end();

            if (!collisionDetected)
            {
                return ssrc;
            }
        }
    }

protected:
    utils::SsrcGenerator _ssrcGenerator;
    std::unique_ptr<memory::PacketPoolAllocator> _allocator;
    std::vector<std::unique_ptr<bridge::SsrcInboundContext>> _ssrcInboundContext;
    std::vector<std::unique_ptr<bridge::SsrcOutboundContext>> ssrcOutboundContexts;
    uint64_t _wallClock;
};

TEST_F(SsrcOutboundContextTest, audioRewrite)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForAudio();
    auto ssrcInboundContext = createInboundContextForAudio();

    memory::Packet packet;

    auto rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 12;
    rtpHeader->timestamp = 5000;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 12, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 12, 5000));

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 13;
    rtpHeader->timestamp = 5000 + 960;
    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 13, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 13, 5000 + 960));
}

TEST_F(SsrcOutboundContextTest, audioRewriteAdvTimestamp)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForAudio();
    auto ssrcInboundContext = createInboundContextForAudio();

    memory::Packet packet;

    auto rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 12;
    rtpHeader->timestamp = 5000;

    const auto seqCount = 12;
    const auto curTimestamp = 5000;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 12, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, seqCount, curTimestamp));

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 13;
    rtpHeader->timestamp = 5000 + 5 * 960;
    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 13, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, seqCount + 1, curTimestamp + 960 * 5));
}

TEST_F(SsrcOutboundContextTest, audioRewritePrevSequenceNumber)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForAudio();
    auto ssrcInboundContext = createInboundContextForAudio();

    memory::Packet packet;

    auto rtpHeader = initializePacket(packet, *ssrcInboundContext);

    rtpHeader->sequenceNumber = 3;
    rtpHeader->timestamp = 5000;
    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 3, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 3, 5000));

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 10;
    rtpHeader->timestamp = 5000 + 7 * 960;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 10, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 10, 5000 + 7 * 960));

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 5;
    rtpHeader->timestamp = 5000 + 2 * 960;
    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 5, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 5, 5000 + 2 * 960));

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 4;
    rtpHeader->timestamp = 5000 + 960;
    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 4, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 4, 5000 + 960));
}

TEST_F(SsrcOutboundContextTest, audioRewriteShouldReturnFalseWhenPacketIsOlderThanSsrcSwitch)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForAudio();
    auto ssrcInboundContext = createInboundContextForAudio();

    memory::Packet packet;

    auto rtpHeader = initializePacket(packet, *ssrcInboundContext);
    uint32_t extSeqNo = 12;

    rtpHeader->sequenceNumber = extSeqNo;
    rtpHeader->timestamp = 5000;

    const auto curTimestamp = 5000;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, extSeqNo, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, extSeqNo, curTimestamp));

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    extSeqNo = 3;
    rtpHeader->sequenceNumber = extSeqNo;
    rtpHeader->timestamp = 5000 - 2 * 960;

    // Before switch
    ASSERT_FALSE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, extSeqNo, _wallClock, false));
}

TEST_F(SsrcOutboundContextTest, audioRewriteSmallGap)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForAudio();
    auto ssrcInboundContext = createInboundContextForAudio();

    memory::Packet packet;

    auto rtpHeader = initializePacket(packet, *ssrcInboundContext);
    uint32_t extSeqNo = 12;

    rtpHeader->sequenceNumber = extSeqNo;
    rtpHeader->timestamp = 5000;

    const auto seqCount = extSeqNo;
    const auto curTimestamp = 5000;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 12, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, seqCount, curTimestamp));

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    extSeqNo = 201;
    rtpHeader->sequenceNumber = extSeqNo;
    rtpHeader->timestamp = 5000 + 189 * 960;
    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, extSeqNo, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, seqCount + 189, curTimestamp + 189 * 960));
}

TEST_F(SsrcOutboundContextTest, audioRewriteLargeGap)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForAudio();
    auto ssrcInboundContext = createInboundContextForAudio();

    memory::Packet packet;

    auto rtpHeader = initializePacket(packet, *ssrcInboundContext);
    uint32_t extSeqNo = 12;

    rtpHeader->sequenceNumber = extSeqNo;
    rtpHeader->timestamp = 5000;

    const auto seqCount = 12;
    const auto curTimestamp = 5000;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, extSeqNo, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, extSeqNo, curTimestamp));

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    _wallClock += 6 * utils::Time::sec;
    extSeqNo = 312;
    rtpHeader->sequenceNumber = extSeqNo;
    rtpHeader->timestamp = 5000 + 300 * 960;
    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, extSeqNo, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, seqCount + 1, curTimestamp + 300 * 960));
}

TEST_F(SsrcOutboundContextTest, audioRewriteSsrcSwitchAfterMute)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForAudio();
    auto ssrcInboundContext0 = createInboundContextForAudio();
    auto ssrcInboundContext1 = createInboundContextForAudio();

    memory::Packet packet0;
    memory::Packet packet1;

    auto rtpHeaderSsrc0 = initializePacket(packet0, *ssrcInboundContext0);
    auto rtpHeaderSsrc1 = initializePacket(packet1, *ssrcInboundContext1);

    const auto curTimestamp = 5000;

    uint32_t extSeqNo = 12;
    rtpHeaderSsrc0->sequenceNumber = extSeqNo;
    rtpHeaderSsrc0->timestamp = curTimestamp;

    const uint32_t outSsrc = ssrcOutboundContext->ssrc;
    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeaderSsrc0, *ssrcInboundContext0, extSeqNo, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeaderSsrc0, outSsrc, extSeqNo, curTimestamp));

    _wallClock += 6 * utils::Time::sec;
    extSeqNo = 0x123A890;
    rtpHeaderSsrc1->sequenceNumber = 0xA890;
    rtpHeaderSsrc1->timestamp = 1235000;
    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeaderSsrc1, *ssrcInboundContext1, extSeqNo, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeaderSsrc1, outSsrc, 13, curTimestamp + 300 * 960));
}

TEST_F(SsrcOutboundContextTest, audioRewriteRewriteAfterDropTelephoneEvents)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForAudio();
    auto ssrcInboundContext = createInboundContextForAudio();

    memory::Packet packet;

    auto rtpHeader = initializePacket(packet, *ssrcInboundContext);

    rtpHeader->sequenceNumber = 12;
    rtpHeader->timestamp = 5000;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 12, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 12, 5000));

    ssrcOutboundContext->dropTelephoneEvent(13, ssrcInboundContext->ssrc);
    ssrcOutboundContext->dropTelephoneEvent(14, ssrcInboundContext->ssrc);
    ssrcOutboundContext->dropTelephoneEvent(15, ssrcInboundContext->ssrc);
    ssrcOutboundContext->dropTelephoneEvent(16, ssrcInboundContext->ssrc);

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 17;
    rtpHeader->timestamp = 5000 + 960;
    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 17, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 13, 5000 + 960));
}

TEST_F(SsrcOutboundContextTest, audioRewriteRewriteAfterOutOfOrderedDropTelephoneEvents)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForAudio();
    auto ssrcInboundContext = createInboundContextForAudio();

    memory::Packet packet;

    auto rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 12;
    rtpHeader->timestamp = 5000;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 12, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 12, 5000));

    ssrcOutboundContext->dropTelephoneEvent(16, ssrcInboundContext->ssrc);
    ssrcOutboundContext->dropTelephoneEvent(13, ssrcInboundContext->ssrc);
    ssrcOutboundContext->dropTelephoneEvent(14, ssrcInboundContext->ssrc);
    ssrcOutboundContext->dropTelephoneEvent(15, ssrcInboundContext->ssrc);

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 17;
    rtpHeader->timestamp = 5000 + 960;
    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 17, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 13, 5000 + 960));
}

TEST_F(SsrcOutboundContextTest, audioRewriteOutOfOrder)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForAudio();
    auto ssrcInboundContext = createInboundContextForAudio();

    memory::Packet packet;

    auto rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 12;
    rtpHeader->timestamp = 5000;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 12, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 12, 5000));

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 15;
    rtpHeader->timestamp = 5000 + 960 * 3;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 15, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 15, 5000 + 960 * 3));

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 13;
    rtpHeader->timestamp = 5000 + 960;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 13, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 13, 5000 + 960));

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 17;
    rtpHeader->timestamp = 5000 + 960 * 5;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 17, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 17, 5000 + 960 * 5));

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 14;
    rtpHeader->timestamp = 5000 + 960 * 2;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 14, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 14, 5000 + 960 * 2));

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 16;
    rtpHeader->timestamp = 5000 + 960 * 4;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 16, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 16, 5000 + 960 * 4));

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 18;
    rtpHeader->timestamp = 5000 + 960 * 6;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 18, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 18, 5000 + 960 * 6));
}

TEST_F(SsrcOutboundContextTest, audioRewriteDropTelephoneEventsOutOfOrder)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForAudio();
    auto ssrcInboundContext = createInboundContextForAudio();

    memory::Packet packet;

    auto rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 12;
    rtpHeader->timestamp = 5000;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 12, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 12, 5000));

    ssrcOutboundContext->dropTelephoneEvent(17, ssrcInboundContext->ssrc);
    ssrcOutboundContext->dropTelephoneEvent(16, ssrcInboundContext->ssrc);

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 13;
    rtpHeader->timestamp = 5000 + 960;

    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 13, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 13, 5000 + 960));

    ssrcOutboundContext->dropTelephoneEvent(18, ssrcInboundContext->ssrc);

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 15;
    rtpHeader->timestamp = 5000 + 960 * 3;

    // This is recoverable as it is later than dropped telephone events but is newer than the last audio packet
    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 15, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 15, 5000 + 960 * 3));

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 14;
    rtpHeader->timestamp = 5000 + 960 * 2;

    // This is not recoverable because is lated than dropped telephone events AND later than last audio packet sent
    ASSERT_FALSE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 14, _wallClock, false));

    rtpHeader = initializePacket(packet, *ssrcInboundContext);
    rtpHeader->sequenceNumber = 19;
    rtpHeader->timestamp = 5000 + 960 * 4;

    // Check if it next packet proceed correctly
    ASSERT_TRUE(ssrcOutboundContext->rewriteAudio(*rtpHeader, *ssrcInboundContext, 19, _wallClock, false));
    ASSERT_TRUE(verifyRtp(*rtpHeader, ssrcOutboundContext->ssrc, 16, 5000 + 960 * 4));
}

TEST_F(SsrcOutboundContextTest, videoRewrite)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoVp8();
    auto ssrcInboundContext0 = createInboundContextForVideoVp8();
    auto ssrcInboundContext1 = createInboundContextForVideoVp8();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examineVp8(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 1, 1, 1, 1, 1, 1, 1, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 1, 1000, 2, 1, 1, 2, 2, _wallClock);
}

TEST_F(SsrcOutboundContextTest, vp8CountersAreConsecutiveWhenSsrcIsUnchanged)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoVp8();
    auto ssrcInboundContext = createInboundContextForVideoVp8();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examineVp8(*ssrcOutboundContext, *ssrcInboundContext, *packet, 1, 1, 1, 1, 1, 1, 1, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext, *packet, 2, 2, 2, 2, 2, 2, 2, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext, *packet, 3, 3, 3, 3, 3, 3, 3, _wallClock);
}

TEST_F(SsrcOutboundContextTest, vp8CountersAreConsecutiveWhenSsrcIsChanged)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoVp8();
    auto ssrcInboundContext0 = createInboundContextForVideoVp8();
    auto ssrcInboundContext1 = createInboundContextForVideoVp8();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examineVp8(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 1, 1, 1, 1, 1, 1, 1, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 2, 2, 2, 2, 2, 2, 2, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 10, 10000, 3, 10, 10, 3, 3, 2, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteCountersAreConsecutiveWhenSsrcIsChangedSequenceNumberLower)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoVp8();
    auto ssrcInboundContext0 = createInboundContextForVideoVp8();
    auto ssrcInboundContext1 = createInboundContextForVideoVp8();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examineVp8(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 1, 1, 1, 1, 1, 1, 1, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 2, 2, 2, 2, 2, 2, 2, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 65535, 10000, 3, 10, 10, 3, 3, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteCountersAreConsecutiveIfLastPacketBeforeSwitchIsReordered)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoVp8();
    auto ssrcInboundContext0 = createInboundContextForVideoVp8();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examineVp8(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 1, 1, 1, 1, 1, 1, 1, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 3, 3, 3, 3, 3, 3, 3, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 2, 2, 2, 2, 2, 2, 2, _wallClock);

    // change ssrc
    auto ssrcInboundContext1 = createInboundContextForVideoVp8();
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 10, 10, 4, 10, 10, 4, 4, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteCountersAreConsecutiveWhenSsrcIsUnchangedAndSequenceRollover)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoVp8();
    auto ssrcInboundContext = createInboundContextForVideoVp8();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examineVp8(*ssrcOutboundContext, *ssrcInboundContext, *packet, 65535, 1, 65535, 1, 1, 1, 1, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext, *packet, 0x10000, 2, 0x10000, 2, 2, 2, 2, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext, *packet, 0x10001, 3, 0x10001, 3, 3, 3, 3, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteCountersAreConsecutiveIfLastPacketBeforeSwitchIsReorderedWithRollover)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoVp8();
    auto ssrcInboundContext0 = createInboundContextForVideoVp8();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examineVp8(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 65534, 1, 65534, 1, 1, 1, 1, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 0x10000, 3, 0x10000u, 3, 3, 3, 3, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 65535, 2, 65535, 2, 2, 2, 2, _wallClock);

    auto ssrcInboundContext1 = createInboundContextForVideoVp8();
    examineVp8(*ssrcOutboundContext,
        *ssrcInboundContext1,
        *packet,
        4711 + 0x10000,
        10,
        0x10001,
        10,
        10,
        4,
        4,
        _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteLongGapInSequenceNumbersSameSsrc)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoVp8();
    auto ssrcInboundContext = createInboundContextForVideoVp8();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examineVp8(*ssrcOutboundContext, *ssrcInboundContext, *packet, 10, 1000, 10, 1, 1, 1, 1, 1000, _wallClock);
    _wallClock += utils::Time::sec * 170;
    examineVp8(*ssrcOutboundContext,
        *ssrcInboundContext,
        *packet,
        17000,
        12000000,
        11,
        3,
        3,
        3,
        3,
        15301000,
        _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteLongGapInSequenceNumbersNewSsrc)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoVp8();
    auto ssrcInboundContext0 = createInboundContextForVideoVp8();
    auto ssrcInboundContext1 = createInboundContextForVideoVp8();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examineVp8(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 10000, 1, 10000, 1, 1, 1, 1, 1, _wallClock);
    _wallClock += utils::Time::sec * 2;
    examineVp8(*ssrcOutboundContext,
        *ssrcInboundContext1,
        *packet,
        30000,
        1000000,
        10001,
        3,
        3,
        2,
        2,
        180001,
        _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteCountersAreConsecutiveWhenSsrcChangeAndReorder)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoVp8();
    auto ssrcInboundContext0 = createInboundContextForVideoVp8();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    uint32_t lastTimestamp = 0;

    examineVp8(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 74, 60000, 74, 32764, 255, 32764, 255, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 75, 60010, 75, 2, 2, 2, 2, _wallClock);

    // change ssrc
    lastTimestamp = rtpHeader->timestamp.get();
    auto ssrcInboundContext1 = createInboundContextForVideoVp8();
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 82, 10000, 76, 10, 10, 3, 3, _wallClock);
    EXPECT_EQ(lastTimestamp, rtpHeader->timestamp.get()); // equal since wallClock did not advance

    // jump 18 packets and 250ms
    examineVp8(*ssrcOutboundContext,
        *ssrcInboundContext1,
        *packet,
        100,
        10000 + 90 * 250,
        94,
        14,
        14,
        7,
        7,
        _wallClock + 250);
    EXPECT_EQ(lastTimestamp + 90 * 250, rtpHeader->timestamp.get());

    // emulate 2 rtx
    examineVp8(*ssrcOutboundContext,
        *ssrcInboundContext1,
        *packet,
        83,
        10000 + 90 * 25,
        77,
        11,
        10,
        4,
        3,
        _wallClock + 250);
    EXPECT_EQ(lastTimestamp + 90 * 25, rtpHeader->timestamp.get());

    // emulate 2 rtx
    examineVp8(*ssrcOutboundContext,
        *ssrcInboundContext1,
        *packet,
        85,
        10000 + 90 * 50,
        79,
        12,
        11,
        5,
        4,
        _wallClock + 250);
    EXPECT_EQ(lastTimestamp + 90 * 50, rtpHeader->timestamp.get());

    // continue rtp
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 101, 10000, 95, 15, 15, 8, 8, _wallClock + 250);
    EXPECT_EQ(lastTimestamp, rtpHeader->timestamp.get());
}

TEST_F(SsrcOutboundContextTest, videoRewriteDropWhenDelayedPacketsArrivesAfterSwitch)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoVp8();
    auto ssrcInboundContext0 = createInboundContextForVideoVp8();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    uint32_t lastTimestamp = 0;

    examineVp8(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 74, 60000, 74, 32764, 255, 32764, 255, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 75, 60010, 75, 2, 2, 2, 2, _wallClock);

    // change ssrc
    lastTimestamp = rtpHeader->timestamp.get();
    auto ssrcInboundContext1 = createInboundContextForVideoVp8();
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 82, 10000, 76, 10, 10, 3, 3, _wallClock);
    EXPECT_EQ(lastTimestamp, rtpHeader->timestamp.get()); // equal since wallClock did not advance

    // emulate 2 rtx
    expectVideoPacketDrop(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 3, 10000 - 9990, 10, 10, _wallClock);
    expectVideoPacketDrop(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 4, 10000 - 9990, 10, 10, _wallClock);

    // continue rtp
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 83, 10000, 77, 10, 10, 3, 3, _wallClock);
    EXPECT_EQ(lastTimestamp, rtpHeader->timestamp.get());
}

TEST_F(SsrcOutboundContextTest, videoRewriteSeqSkipWithinMargin)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoVp8();
    auto ssrcInboundContext = createInboundContextForVideoVp8();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examineVp8(*ssrcOutboundContext, *ssrcInboundContext, *packet, 1, 1, 1, 1, 1, 1, 1, _wallClock);
    examineVp8(*ssrcOutboundContext,
        *ssrcInboundContext,
        *packet,
        MAX_VIDEO_SEQ_GAP,
        2,
        MAX_VIDEO_SEQ_GAP,
        2,
        2,
        2,
        2,
        _wallClock);
    examineVp8(*ssrcOutboundContext,
        *ssrcInboundContext,
        *packet,
        MAX_VIDEO_SEQ_GAP + 1,
        3,
        MAX_VIDEO_SEQ_GAP + 1,
        3,
        3,
        3,
        3,
        _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteSeqSkipWithinMarginRollover)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoVp8();
    auto ssrcInboundContext = createInboundContextForVideoVp8();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examineVp8(*ssrcOutboundContext, *ssrcInboundContext, *packet, 0xFFF0, 1, 0xFFF0, 1, 1, 1, 1, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext, *packet, 0x13F90, 2, 0xFFF0 + 1, 2, 2, 2, 2, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext, *packet, 0x13F90 + 3, 3, 0xFFF0 + 4, 3, 3, 3, 3, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteSeqSkipBeyondMargin)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoVp8();
    auto ssrcInboundContext = createInboundContextForVideoVp8();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examineVp8(*ssrcOutboundContext, *ssrcInboundContext, *packet, 1, 1, 1, 1, 1, 1, 1, _wallClock);
    examineVp8(*ssrcOutboundContext,
        *ssrcInboundContext,
        *packet,
        MAX_VIDEO_SEQ_GAP + 10,
        2,
        2,
        2,
        2,
        2,
        2,
        _wallClock);
    examineVp8(*ssrcOutboundContext,
        *ssrcInboundContext,
        *packet,
        MAX_VIDEO_SEQ_GAP + 11,
        3,
        3,
        3,
        3,
        3,
        3,
        _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteSeqSkipBeyondMarginRollover)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoVp8();
    auto ssrcInboundContext = createInboundContextForVideoVp8();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examineVp8(*ssrcOutboundContext, *ssrcInboundContext, *packet, 0xFFF0, 1, 0xFFF0, 1, 1, 1, 1, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext, *packet, 0x14002, 2, 0xFFF1, 2, 2, 2, 2, _wallClock);
    examineVp8(*ssrcOutboundContext, *ssrcInboundContext, *packet, 0x14003, 3, 0xFFF2, 3, 3, 3, 3, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteH264)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoH264();
    auto ssrcInboundContext0 = createInboundContextForVideoH264();
    auto ssrcInboundContext1 = createInboundContextForVideoH264();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 1, 1, 1, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 1, 1000, 2, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteCountersAreConsecutiveWhenSsrcIsUnchangedH264)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoH264();
    auto ssrcInboundContext = createInboundContextForVideoH264();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, 1, 1, 1, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, 2, 2, 2, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, 3, 3, 3, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteCountersAreConsecutiveWhenSsrcIsChangedH264)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoH264();
    auto ssrcInboundContext0 = createInboundContextForVideoH264();
    auto ssrcInboundContext1 = createInboundContextForVideoH264();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 1, 1, 1, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 2, 2, 2, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 10, 10000, 3, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteCountersAreConsecutiveWhenSsrcIsChangedSequenceNumberLowerH264)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoH264();
    auto ssrcInboundContext0 = createInboundContextForVideoH264();
    auto ssrcInboundContext1 = createInboundContextForVideoH264();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 1, 1, 1, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 2, 2, 2, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 65535, 10000, 3, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteCountersAreConsecutiveIfLastPacketBeforeSwitchIsReorderedH264)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoH264();
    auto ssrcInboundContext0 = createInboundContextForVideoH264();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 1, 1, 1, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 3, 3, 3, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 2, 2, 2, _wallClock);

    // change ssrc
    auto ssrcInboundContext1 = createInboundContextForVideoH264();
    examineH264(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 10, 10, 4, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteCountersAreConsecutiveWhenSsrcIsUnchangedAndSequenceRolloverH264)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoH264();
    auto ssrcInboundContext = createInboundContextForVideoH264();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, 65535, 1, 65535, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, 0x10000, 2, 0x10000, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, 0x10001, 3, 0x10001, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteCountersAreConsecutiveIfLastPacketBeforeSwitchIsReorderedWithRolloverH264)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoH264();
    auto ssrcInboundContext0 = createInboundContextForVideoH264();
    auto ssrcInboundContext1 = createInboundContextForVideoH264();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 65534, 1, 65534, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 0x10000, 3, 0x10000u, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 65535, 2, 65535, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 4711 + 0x10000, 10, 0x10001, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteLongGapInSequenceNumbersSameSsrcH264)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoH264();
    auto ssrcInboundContext = createInboundContextForVideoH264();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, 10000, 1, 10000, _wallClock);
    _wallClock += utils::Time::sec * 170;
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, 17000, 10000, 10001, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteLongGapInSequenceNumbersNewSsrcH264)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoH264();
    auto ssrcInboundContext0 = createInboundContextForVideoH264();
    auto ssrcInboundContext1 = createInboundContextForVideoH264();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 10000, 1, 10000, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 30000, 1, 10001, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteCountersAreConsecutiveWhenSsrcChangeAndReorderH264)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoH264();
    auto ssrcInboundContext0 = createInboundContextForVideoH264();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);

    uint32_t lastTimestamp = 0;

    examineH264(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 74, 60000, 74, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext0, *packet, 75, 60010, 75, _wallClock);

    // change ssrc
    lastTimestamp = rtpHeader->timestamp.get();
    auto ssrcInboundContext1 = createInboundContextForVideoH264();
    examineH264(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 82, 10000, 76, _wallClock);
    EXPECT_EQ(lastTimestamp, rtpHeader->timestamp.get()); // equal since wallClock did not advance

    // jump 18 packets and 250ms
    examineH264(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 100, 10000 + 90 * 250, 94, _wallClock + 250);
    EXPECT_EQ(lastTimestamp + 90 * 250, rtpHeader->timestamp.get());

    // emulate 2 rtx
    examineH264(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 83, 10000 + 90 * 25, 77, _wallClock + 250);
    EXPECT_EQ(lastTimestamp + 90 * 25, rtpHeader->timestamp.get());

    // emulate 2 rtx
    examineH264(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 85, 10000 + 90 * 50, 79, _wallClock + 250);
    EXPECT_EQ(lastTimestamp + 90 * 50, rtpHeader->timestamp.get());

    // continue rtp
    examineH264(*ssrcOutboundContext, *ssrcInboundContext1, *packet, 101, 10000, 95, _wallClock + 250);
    EXPECT_EQ(lastTimestamp, rtpHeader->timestamp.get());
}

TEST_F(SsrcOutboundContextTest, videoRewriteSeqSkipWithinMarginH264)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoH264();
    auto ssrcInboundContext = createInboundContextForVideoH264();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, 1, 1, 1, _wallClock);
    examineH264(*ssrcOutboundContext,
        *ssrcInboundContext,
        *packet,
        MAX_VIDEO_SEQ_GAP,
        2,
        MAX_VIDEO_SEQ_GAP,
        _wallClock);
    examineH264(*ssrcOutboundContext,
        *ssrcInboundContext,
        *packet,
        MAX_VIDEO_SEQ_GAP + 1,
        3,
        MAX_VIDEO_SEQ_GAP + 1,
        _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteSeqSkipWithinMarginRolloverH264)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoH264();
    auto ssrcInboundContext = createInboundContextForVideoH264();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, 0xFFF0, 1, 0xFFF0, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, 0x13F90, 2, 0xFFF0 + 1, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, 0x13F90 + 3, 3, 0xFFF0 + 4, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteSeqSkipBeyondMarginH264)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoH264();
    auto ssrcInboundContext = createInboundContextForVideoH264();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, 1, 1, 1, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, MAX_VIDEO_SEQ_GAP + 10, 2, 2, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, MAX_VIDEO_SEQ_GAP + 11, 3, 3, _wallClock);
}

TEST_F(SsrcOutboundContextTest, videoRewriteSeqSkipBeyondMarginRolloverH264)
{
    auto ssrcOutboundContext = createDefaultOutboundContextForVideoH264();
    auto ssrcInboundContext = createInboundContextForVideoH264();

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, 0xFFF0, 1, 0xFFF0, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, 0x14002, 2, 0xFFF1, _wallClock);
    examineH264(*ssrcOutboundContext, *ssrcInboundContext, *packet, 0x14003, 3, 0xFFF2, _wallClock);
}

/** MOVE TEST for the right place

 * TEST_F(SsrcOutboundContextTest, videoRewriteRtx)
{
    const uint8_t vp8PayloadType = 100;
    // Make RTP packet
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size - sizeof(uint16_t));

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    rtpHeader->ssrc = 1;
    rtpHeader->sequenceNumber = 1;
    rtpHeader->timestamp = 1;

    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    // Make RTX packet from above packet
    const auto headerLength = rtpHeader->headerLength();
    auto rtxPacket = memory::makeUniquePacket(*_allocator);
    memcpy(rtxPacket->get(), packet->get(), headerLength);
    auto copyHead = rtxPacket->get() + headerLength;
    reinterpret_cast<uint16_t*>(copyHead)[0] = hton<uint16_t>(rtpHeader->sequenceNumber.get());
    copyHead += sizeof(uint16_t);
    memcpy(copyHead, payload, packet->getLength() - headerLength);
    rtxPacket->setLength(packet->getLength() + sizeof(uint16_t));

    const bridge::RtpMap rtxRtpMap(bridge::RtpMap::Format::RTX);
    auto rtxHeader = rtp::RtpHeader::fromPacket(*rtxPacket);
    rtxHeader->ssrc = 2;
    rtxHeader->sequenceNumber = 2;
    rtxHeader->payloadType = rtxRtpMap.payloadType;

    // Rewrite RTX
    const auto originalSequenceNumber =
        bridge::RtpVideoRewriter::rewriteRtxPacket(*rtxPacket, 1, vp8PayloadType, "transport");

    // Validate
    auto rewrittenRtpHeader = rtp::RtpHeader::fromPacket(*rtxPacket);
    EXPECT_EQ(1, originalSequenceNumber);
    EXPECT_EQ(1, rewrittenRtpHeader->sequenceNumber.get());
    EXPECT_EQ(1, rewrittenRtpHeader->ssrc.get());
    EXPECT_EQ(vp8PayloadType, rewrittenRtpHeader->payloadType);
}


TEST_F(SsrcOutboundContextTest, videoRewriteRingDifference)
{
    uint16_t sequenceNumber0 = 22787;
    uint16_t sequenceNumber1 = 2112;

    const auto offset1 = math::ringDifference<uint16_t>(sequenceNumber1, sequenceNumber0);
    EXPECT_EQ(20675, offset1);
    EXPECT_EQ(offset1 + sequenceNumber1, sequenceNumber0);

    const auto offset2 = math::ringDifference<uint16_t>(sequenceNumber0, sequenceNumber1);
    EXPECT_EQ(-20675, offset2);
    EXPECT_EQ(offset2 + sequenceNumber0, sequenceNumber1);

    {
        const auto offset = math::ringDifference<uint32_t, 12u>(0xFFF, 444);
        EXPECT_EQ(offset, 445);
        EXPECT_EQ((offset + 0xFFF) & 0xFFF, 444);
    }
    {
        const auto offset = math::ringDifference<uint32_t, 12u>(445, 444);
        EXPECT_EQ(offset, -1);
    }
    {
        const auto offset = math::ringDifference<uint32_t, 12u>(845, 844);
        EXPECT_EQ(offset, -1);
        EXPECT_EQ(offset + 845, 844);
    }

    int32_t diffA = math::ringDifference<uint32_t, 12>(100, 100 + (1 << 10));
    EXPECT_EQ(diffA, int32_t(1 << 10));
    int32_t diffB = math::ringDifference<uint32_t, 12>(100, 100 + (1 << 10) + 1);
    EXPECT_EQ(diffB, (1 << 10) + 1);

    {
        const auto offset = math::ringDifference<uint16_t>(0xFFFF, 444);
        EXPECT_EQ(offset, 445);
    }

    {
        const auto offset = math::ringDifference<uint16_t>(888, 444);
        EXPECT_EQ(offset, -444);
    }
}

TEST_F(SsrcOutboundContextTest, videoRewriteFullRing)
{
    int32_t pattern[] = {0, 1, 2, 3, -4, -3, -2, -1};
    int32_t offset[8 * 8];

    for (uint32_t i = 0; i < 8; ++i)
    {
        for (uint32_t j = 0; j < 8; ++j)
        {
            offset[i * 8 + j] = math::ringDifference<uint32_t, 3>(i, (j + i) % 8);
        }
    }

    for (int i = 0; i < 64; ++i)
    {
        EXPECT_EQ(offset[i], pattern[i % 8]);
    }
}

*/
