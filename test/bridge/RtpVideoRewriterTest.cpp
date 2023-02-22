#include "bridge/engine/RtpVideoRewriter.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/Vp8Header.h"
#include "memory/PacketPoolAllocator.h"
#include <array>
#include <gtest/gtest.h>
#include <memory>

namespace
{

const uint32_t outboundSsrc = 12345;

void examine(bridge::SsrcOutboundContext& outboundContext,
    memory::Packet& packet,
    uint32_t ssrc,
    uint32_t seqNo,
    uint32_t timestamp,
    uint32_t expectedSeqNo,
    uint16_t picId,
    uint16_t picIdx,
    uint16_t expectedPicId,
    uint16_t expectedPicIdx)
{
    auto rtpHeader = rtp::RtpHeader::create(packet);
    auto payload = rtpHeader->getPayload();
    rtpHeader->ssrc = ssrc;
    rtpHeader->sequenceNumber = seqNo & 0xFFFFu;
    rtpHeader->timestamp = timestamp;
    codec::Vp8Header::setPicId(payload, picId);
    codec::Vp8Header::setTl0PicIdx(payload, picIdx);

    uint32_t sequenceNumberAfterRewrite = 0;
    bridge::RtpVideoRewriter::rewriteVp8(outboundContext, packet, seqNo, "", sequenceNumberAfterRewrite);

    EXPECT_EQ(outboundContext.ssrc, rtpHeader->ssrc.get());
    EXPECT_EQ(expectedSeqNo & 0xFFFFu, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(expectedSeqNo, sequenceNumberAfterRewrite);
    EXPECT_EQ(expectedPicId, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(expectedPicIdx, codec::Vp8Header::getTl0PicIdx(payload));
}

void examineH264(bridge::SsrcOutboundContext& outboundContext,
    memory::Packet& packet,
    uint32_t ssrc,
    uint32_t seqNo,
    uint32_t timestamp,
    uint32_t expectedSeqNo)
{
    auto rtpHeader = rtp::RtpHeader::create(packet);
    rtpHeader->ssrc = ssrc;
    rtpHeader->sequenceNumber = seqNo & 0xFFFFu;
    rtpHeader->timestamp = timestamp;

    uint32_t sequenceNumberAfterRewrite = 0;
    bridge::RtpVideoRewriter::rewriteH264(outboundContext, packet, seqNo, "", sequenceNumberAfterRewrite);

    EXPECT_EQ(outboundContext.ssrc, rtpHeader->ssrc.get());
    EXPECT_EQ(expectedSeqNo & 0xFFFFu, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(expectedSeqNo, sequenceNumberAfterRewrite);
}

} // namespace

class RtpVideoRewriterTest : public ::testing::Test
{
    void SetUp() override
    {
        _allocator = std::make_unique<memory::PacketPoolAllocator>(16, "RtpVideoRewriterTest");
        _ssrcOutboundContext = std::make_unique<bridge::SsrcOutboundContext>(outboundSsrc,
            *_allocator,
            bridge::RtpMap(bridge::RtpMap::Format::VP8));
    }
    void TearDown() override { _ssrcOutboundContext.reset(); }

protected:
    std::unique_ptr<memory::PacketPoolAllocator> _allocator;
    std::unique_ptr<bridge::SsrcOutboundContext> _ssrcOutboundContext;
};

TEST_F(RtpVideoRewriterTest, ringDifference)
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

TEST_F(RtpVideoRewriterTest, fullRing)
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

TEST_F(RtpVideoRewriterTest, rewrite)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examine(*_ssrcOutboundContext, *packet, 1, 1, 1, 1, 1, 1, 1, 1);
    examine(*_ssrcOutboundContext, *packet, 2, 1, 1000, 2, 1, 1, 2, 2);
}

TEST_F(RtpVideoRewriterTest, rewriteRtx)
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

TEST_F(RtpVideoRewriterTest, countersAreConsecutiveWhenSsrcIsUnchanged)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examine(*_ssrcOutboundContext, *packet, 1, 1, 1, 1, 1, 1, 1, 1);
    examine(*_ssrcOutboundContext, *packet, 1, 2, 2, 2, 2, 2, 2, 2);
    examine(*_ssrcOutboundContext, *packet, 1, 3, 3, 3, 3, 3, 3, 3);
}

TEST_F(RtpVideoRewriterTest, countersAreConsecutiveWhenSsrcIsChanged)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examine(*_ssrcOutboundContext, *packet, 1, 1, 1, 1, 1, 1, 1, 1);
    examine(*_ssrcOutboundContext, *packet, 1, 2, 2, 2, 2, 2, 2, 2);
    examine(*_ssrcOutboundContext, *packet, 2, 10, 10000, 3, 10, 10, 3, 3);
}

TEST_F(RtpVideoRewriterTest, countersAreConsecutiveWhenSsrcIsChangedSequenceNumberLower)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examine(*_ssrcOutboundContext, *packet, 1, 1, 1, 1, 1, 1, 1, 1);
    examine(*_ssrcOutboundContext, *packet, 1, 2, 2, 2, 2, 2, 2, 2);
    examine(*_ssrcOutboundContext, *packet, 2, 65535, 10000, 3, 10, 10, 3, 3);
}

TEST_F(RtpVideoRewriterTest, countersAreConsecutiveIfLastPacketBeforeSwitchIsReordered)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examine(*_ssrcOutboundContext, *packet, 1, 1, 1, 1, 1, 1, 1, 1);
    examine(*_ssrcOutboundContext, *packet, 1, 3, 3, 3, 3, 3, 3, 3);
    examine(*_ssrcOutboundContext, *packet, 1, 2, 2, 2, 2, 2, 2, 2);

    // change ssrc
    examine(*_ssrcOutboundContext, *packet, 2, 10, 10, 4, 10, 10, 4, 4);
}

TEST_F(RtpVideoRewriterTest, countersAreConsecutiveWhenSsrcIsUnchangedAndSequenceRollover)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examine(*_ssrcOutboundContext, *packet, 1, 65535, 1, 65535, 1, 1, 1, 1);

    examine(*_ssrcOutboundContext, *packet, 1, 0x10000, 2, 0x10000, 2, 2, 2, 2);

    examine(*_ssrcOutboundContext, *packet, 1, 0x10001, 3, 0x10001, 3, 3, 3, 3);
}

TEST_F(RtpVideoRewriterTest, countersAreConsecutiveIfLastPacketBeforeSwitchIsReorderedWithRollover)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examine(*_ssrcOutboundContext, *packet, 1, 65534, 1, 65534, 1, 1, 1, 1);
    examine(*_ssrcOutboundContext, *packet, 1, 0x10000, 3, 0x10000u, 3, 3, 3, 3);
    examine(*_ssrcOutboundContext, *packet, 1, 65535, 2, 65535, 2, 2, 2, 2);

    examine(*_ssrcOutboundContext, *packet, 2, 4711 + 0x10000, 10, 0x10001, 10, 10, 4, 4);
}

TEST_F(RtpVideoRewriterTest, longGapInSequenceNumbersSameSsrc)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examine(*_ssrcOutboundContext, *packet, 1, 10000, 1, 10000, 1, 1, 1, 1);
    examine(*_ssrcOutboundContext, *packet, 1, 10, 1000, 10, 3, 3, 3, 3);
}

TEST_F(RtpVideoRewriterTest, longGapInSequenceNumbersNewSsrc)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examine(*_ssrcOutboundContext, *packet, 1, 10000, 1, 10000, 1, 1, 1, 1);
    examine(*_ssrcOutboundContext, *packet, 2, 30000, 1, 10001, 3, 3, 2, 2);
}

TEST_F(RtpVideoRewriterTest, countersAreConsecutiveWhenSsrcChangeAndRtx)
{
    memory::PacketPoolAllocator packetAllocator(512, "test");
    bridge::RtpMap map1(bridge::RtpMap::Format::VP8);
    bridge::SsrcOutboundContext outboundContext(outboundSsrc, packetAllocator, map1);

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    uint32_t lastTimestamp = 0;

    examine(outboundContext, *packet, 1, 74, 60000, 74, 32764, 255, 32764, 255);
    examine(outboundContext, *packet, 1, 75, 60010, 75, 2, 2, 2, 2);

    // change ssrc
    lastTimestamp = rtpHeader->timestamp.get();
    examine(outboundContext, *packet, 2, 82, 10000, 76, 10, 10, 3, 3);
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());

    // emulate 2 rtx
    examine(outboundContext, *packet, 2, 3, 10000, -3, 10, 10, 3, 3);
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());

    examine(outboundContext, *packet, 2, 4, 10000, -2, 10, 10, 3, 3);
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());

    // continue rtp
    examine(outboundContext, *packet, 2, 83, 10000, 77, 10, 10, 3, 3);
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());
}

TEST_F(RtpVideoRewriterTest, seqSkipWithinMargin)
{
    constexpr int32_t MAX_JUMP_AHEAD = 0x10000 / 4;

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examine(*_ssrcOutboundContext, *packet, 1, 1, 1, 1, 1, 1, 1, 1);
    examine(*_ssrcOutboundContext, *packet, 1, MAX_JUMP_AHEAD, 2, MAX_JUMP_AHEAD, 2, 2, 2, 2);
    examine(*_ssrcOutboundContext, *packet, 1, MAX_JUMP_AHEAD + 1, 3, MAX_JUMP_AHEAD + 1, 3, 3, 3, 3);
}

TEST_F(RtpVideoRewriterTest, seqSkipWithinMarginRollover)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examine(*_ssrcOutboundContext, *packet, 1, 0xFFF0, 1, 0xFFF0, 1, 1, 1, 1);
    examine(*_ssrcOutboundContext, *packet, 1, 0x13F90, 2, 0x13F90, 2, 2, 2, 2);
    examine(*_ssrcOutboundContext, *packet, 1, 0x13F92 + 1, 3, 0x13F92 + 1, 3, 3, 3, 3);
}

TEST_F(RtpVideoRewriterTest, seqSkipBeyondMargin)
{
    constexpr int32_t MAX_JUMP_AHEAD = 0x10000 / 4;

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examine(*_ssrcOutboundContext, *packet, 1, 1, 1, 1, 1, 1, 1, 1);
    examine(*_ssrcOutboundContext, *packet, 1, MAX_JUMP_AHEAD + 10, 2, 2, 2, 2, 2, 2);
    examine(*_ssrcOutboundContext, *packet, 1, MAX_JUMP_AHEAD + 11, 3, 3, 3, 3, 3, 3);
}

TEST_F(RtpVideoRewriterTest, seqSkipBeyondMarginRollover)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    examine(*_ssrcOutboundContext, *packet, 1, 0xFFF0, 1, 0xFFF0, 1, 1, 1, 1);
    examine(*_ssrcOutboundContext, *packet, 1, 0x14002, 2, 0xFFF1, 2, 2, 2, 2);
    examine(*_ssrcOutboundContext, *packet, 1, 0x14003, 3, 0xFFF2, 3, 3, 3, 3);
}

TEST_F(RtpVideoRewriterTest, rewriteH264)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*_ssrcOutboundContext, *packet, 1, 1, 1, 1);
    examineH264(*_ssrcOutboundContext, *packet, 2, 1, 1000, 2);
}

TEST_F(RtpVideoRewriterTest, countersAreConsecutiveWhenSsrcIsUnchangedH264)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*_ssrcOutboundContext, *packet, 1, 1, 1, 1);
    examineH264(*_ssrcOutboundContext, *packet, 1, 2, 2, 2);
    examineH264(*_ssrcOutboundContext, *packet, 1, 3, 3, 3);
}

TEST_F(RtpVideoRewriterTest, countersAreConsecutiveWhenSsrcIsChangedH264)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*_ssrcOutboundContext, *packet, 1, 1, 1, 1);
    examineH264(*_ssrcOutboundContext, *packet, 1, 2, 2, 2);
    examineH264(*_ssrcOutboundContext, *packet, 2, 10, 10000, 3);
}

TEST_F(RtpVideoRewriterTest, countersAreConsecutiveWhenSsrcIsChangedSequenceNumberLowerH264)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*_ssrcOutboundContext, *packet, 1, 1, 1, 1);
    examineH264(*_ssrcOutboundContext, *packet, 1, 2, 2, 2);
    examineH264(*_ssrcOutboundContext, *packet, 2, 65535, 10000, 3);
}

TEST_F(RtpVideoRewriterTest, countersAreConsecutiveIfLastPacketBeforeSwitchIsReorderedH264)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*_ssrcOutboundContext, *packet, 1, 1, 1, 1);
    examineH264(*_ssrcOutboundContext, *packet, 1, 3, 3, 3);
    examineH264(*_ssrcOutboundContext, *packet, 1, 2, 2, 2);

    // change ssrc
    examineH264(*_ssrcOutboundContext, *packet, 2, 10, 10, 4);
}

TEST_F(RtpVideoRewriterTest, countersAreConsecutiveWhenSsrcIsUnchangedAndSequenceRolloverH264)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*_ssrcOutboundContext, *packet, 1, 65535, 1, 65535);
    examineH264(*_ssrcOutboundContext, *packet, 1, 0x10000, 2, 0x10000);
    examineH264(*_ssrcOutboundContext, *packet, 1, 0x10001, 3, 0x10001);
}

TEST_F(RtpVideoRewriterTest, countersAreConsecutiveIfLastPacketBeforeSwitchIsReorderedWithRolloverH264)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*_ssrcOutboundContext, *packet, 1, 65534, 1, 65534);
    examineH264(*_ssrcOutboundContext, *packet, 1, 0x10000, 3, 0x10000u);
    examineH264(*_ssrcOutboundContext, *packet, 1, 65535, 2, 65535);
    examineH264(*_ssrcOutboundContext, *packet, 2, 4711 + 0x10000, 10, 0x10001);
}

TEST_F(RtpVideoRewriterTest, longGapInSequenceNumbersSameSsrcH264)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*_ssrcOutboundContext, *packet, 1, 10000, 1, 10000);
    examineH264(*_ssrcOutboundContext, *packet, 1, 10, 1000, 10);
}

TEST_F(RtpVideoRewriterTest, longGapInSequenceNumbersNewSsrcH264)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*_ssrcOutboundContext, *packet, 1, 10000, 1, 10000);
    examineH264(*_ssrcOutboundContext, *packet, 2, 30000, 1, 10001);
}

TEST_F(RtpVideoRewriterTest, countersAreConsecutiveWhenSsrcChangeAndRtxH264)
{
    memory::PacketPoolAllocator packetAllocator(512, "test");
    bridge::RtpMap map1(bridge::RtpMap::Format::VP8);
    bridge::SsrcOutboundContext outboundContext(outboundSsrc, packetAllocator, map1);

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);

    examineH264(outboundContext, *packet, 1, 74, 60000, 74);
    examineH264(outboundContext, *packet, 1, 75, 60010, 75);

    // change ssrc
    const auto lastTimestamp = rtpHeader->timestamp.get();
    examineH264(outboundContext, *packet, 2, 82, 10000, 76);
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());

    // emulate 2 rtx
    examineH264(outboundContext, *packet, 2, 3, 10000, -3);
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());

    examineH264(outboundContext, *packet, 2, 4, 10000, -2);
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());

    // continue rtp
    examineH264(outboundContext, *packet, 2, 83, 10000, 77);
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());
}

TEST_F(RtpVideoRewriterTest, seqSkipWithinMarginH264)
{
    constexpr int32_t MAX_JUMP_AHEAD = 0x10000 / 4;

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*_ssrcOutboundContext, *packet, 1, 1, 1, 1);
    examineH264(*_ssrcOutboundContext, *packet, 1, MAX_JUMP_AHEAD, 2, MAX_JUMP_AHEAD);
    examineH264(*_ssrcOutboundContext, *packet, 1, MAX_JUMP_AHEAD + 1, 3, MAX_JUMP_AHEAD + 1);
}

TEST_F(RtpVideoRewriterTest, seqSkipWithinMarginRolloverH264)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*_ssrcOutboundContext, *packet, 1, 0xFFF0, 1, 0xFFF0);
    examineH264(*_ssrcOutboundContext, *packet, 1, 0x13F90, 2, 0x13F90);
    examineH264(*_ssrcOutboundContext, *packet, 1, 0x13F92 + 1, 3, 0x13F92 + 1);
}

TEST_F(RtpVideoRewriterTest, seqSkipBeyondMarginH264)
{
    constexpr int32_t MAX_JUMP_AHEAD = 0x10000 / 4;

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*_ssrcOutboundContext, *packet, 1, 1, 1, 1);
    examineH264(*_ssrcOutboundContext, *packet, 1, MAX_JUMP_AHEAD + 10, 2, 2);
    examineH264(*_ssrcOutboundContext, *packet, 1, MAX_JUMP_AHEAD + 11, 3, 3);
}

TEST_F(RtpVideoRewriterTest, seqSkipBeyondMarginRolloverH264)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);
    examineH264(*_ssrcOutboundContext, *packet, 1, 0xFFF0, 1, 0xFFF0);
    examineH264(*_ssrcOutboundContext, *packet, 1, 0x14002, 2, 0xFFF1);
    examineH264(*_ssrcOutboundContext, *packet, 1, 0x14003, 3, 0xFFF2);
}
