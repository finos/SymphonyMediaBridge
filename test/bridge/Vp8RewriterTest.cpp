#include "bridge/engine/Vp8Rewriter.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "codec/Vp8Header.h"
#include "memory/PacketPoolAllocator.h"
#include "utils/OutboundSequenceNumber.h"
#include <array>
#include <gtest/gtest.h>
#include <memory>

namespace
{

static const uint32_t outboundSsrc = 12345;

} // namespace

class Vp8RewriterTest : public ::testing::Test
{
    void SetUp() override
    {
        _allocator = std::make_unique<memory::PacketPoolAllocator>(16, "Vp8RewriterTest");
        _ssrcOutboundContext = std::make_unique<bridge::SsrcOutboundContext>(outboundSsrc,
            *_allocator,
            bridge::RtpMap(bridge::RtpMap::Format::VP8));
    }
    void TearDown() override { _ssrcOutboundContext.reset(); }

protected:
    std::unique_ptr<memory::PacketPoolAllocator> _allocator;
    std::unique_ptr<bridge::SsrcOutboundContext> _ssrcOutboundContext;
};

TEST_F(Vp8RewriterTest, getOffset)
{
    uint16_t sequenceNumber0 = 22787;
    uint16_t sequenceNumber1 = 2112;

    const int32_t offset1 = utils::Offset::getOffset<int32_t, 16>(sequenceNumber0, sequenceNumber1);
    EXPECT_EQ(20675, offset1);

    const int32_t offset2 = utils::Offset::getOffset<int32_t, 16>(sequenceNumber1, sequenceNumber0);
    EXPECT_EQ(-20675, offset2);
}

TEST_F(Vp8RewriterTest, rewrite)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    rtpHeader->ssrc = 1;
    rtpHeader->sequenceNumber = 1;
    rtpHeader->timestamp = 1;

    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    uint32_t rewrittenExtendedSequenceNumber = 0;
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        rtpHeader->sequenceNumber.get(),
        "transport-1",
        rewrittenExtendedSequenceNumber);
    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(2, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(2, rewrittenExtendedSequenceNumber);
    EXPECT_EQ(11194, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(212, codec::Vp8Header::getTl0PicIdx(payload));
    auto lastTimestamp = rtpHeader->timestamp.get();

    rtpHeader->ssrc = 2;
    rtpHeader->sequenceNumber = 1000;
    rtpHeader->timestamp = 1000;
    codec::Vp8Header::setPicId(payload, 1);
    codec::Vp8Header::setTl0PicIdx(payload, 1);

    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        rtpHeader->sequenceNumber.get(),
        "transport-1",
        rewrittenExtendedSequenceNumber);
    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(3, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(3, rewrittenExtendedSequenceNumber);
    EXPECT_EQ(11195, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(213, codec::Vp8Header::getTl0PicIdx(payload));
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());
}

TEST_F(Vp8RewriterTest, rewriteRtx)
{
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

    const bridge::RtpMap rtxRtpMap(bridge::RtpMap::Format::VP8RTX);
    auto rtxHeader = rtp::RtpHeader::fromPacket(*rtxPacket);
    rtxHeader->ssrc = 2;
    rtxHeader->sequenceNumber = 2;
    rtxHeader->payloadType = rtxRtpMap.payloadType;

    // Rewrite RTX
    const auto originalSequenceNumber = bridge::Vp8Rewriter::rewriteRtxPacket(*rtxPacket, 1, "transport");

    // Validate
    auto rewrittenRtpHeader = rtp::RtpHeader::fromPacket(*rtxPacket);
    EXPECT_EQ(1, originalSequenceNumber);
    EXPECT_EQ(1, rewrittenRtpHeader->sequenceNumber.get());
    EXPECT_EQ(1, rewrittenRtpHeader->ssrc.get());
    EXPECT_EQ(rtxRtpMap.payloadType, rewrittenRtpHeader->payloadType);
}

TEST_F(Vp8RewriterTest, countersAreConsecutiveWhenSsrcIsUnchanged)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    rtpHeader->ssrc = 1;
    rtpHeader->sequenceNumber = 1;
    rtpHeader->timestamp = 1;
    codec::Vp8Header::setPicId(payload, 1);
    codec::Vp8Header::setTl0PicIdx(payload, 1);
    uint32_t rewrittenExtendedSequenceNumber = 0;
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        rtpHeader->sequenceNumber.get(),
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(2, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(2, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(2, codec::Vp8Header::getTl0PicIdx(payload));
    auto lastTimestamp = rtpHeader->timestamp.get();

    rtpHeader->ssrc = 1;
    rtpHeader->sequenceNumber = 2;
    rtpHeader->timestamp = 2;
    codec::Vp8Header::setPicId(payload, 2);
    codec::Vp8Header::setTl0PicIdx(payload, 2);
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        rtpHeader->sequenceNumber.get(),
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(3, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(3, rewrittenExtendedSequenceNumber);
    EXPECT_EQ(3, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(3, codec::Vp8Header::getTl0PicIdx(payload));
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());
    lastTimestamp = rtpHeader->timestamp.get();

    rtpHeader->ssrc = 1;
    rtpHeader->sequenceNumber = 3;
    rtpHeader->timestamp = 3;
    codec::Vp8Header::setPicId(payload, 3);
    codec::Vp8Header::setTl0PicIdx(payload, 3);
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        rtpHeader->sequenceNumber.get(),
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(4, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(4, rewrittenExtendedSequenceNumber);
    EXPECT_EQ(4, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(4, codec::Vp8Header::getTl0PicIdx(payload));
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());
}

TEST_F(Vp8RewriterTest, countersAreConsecutiveWhenSsrcIsChanged)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    rtpHeader->ssrc = 1;
    rtpHeader->sequenceNumber = 1;
    rtpHeader->timestamp = 1;
    codec::Vp8Header::setPicId(payload, 1);
    codec::Vp8Header::setTl0PicIdx(payload, 1);
    uint32_t rewrittenExtendedSequenceNumber = 0;
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        rtpHeader->sequenceNumber.get(),
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(2, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(2, rewrittenExtendedSequenceNumber);
    EXPECT_EQ(2, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(2, codec::Vp8Header::getTl0PicIdx(payload));
    auto lastTimestamp = rtpHeader->timestamp.get();

    rtpHeader->ssrc = 1;
    rtpHeader->sequenceNumber = 2;
    rtpHeader->timestamp = 2;
    codec::Vp8Header::setPicId(payload, 2);
    codec::Vp8Header::setTl0PicIdx(payload, 2);
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        rtpHeader->sequenceNumber.get(),
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(3, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(3, rewrittenExtendedSequenceNumber);
    EXPECT_EQ(3, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(3, codec::Vp8Header::getTl0PicIdx(payload));
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());
    lastTimestamp = rtpHeader->timestamp.get();

    rtpHeader->ssrc = 2;
    rtpHeader->sequenceNumber = 10;
    rtpHeader->timestamp = 10000;
    codec::Vp8Header::setPicId(payload, 10);
    codec::Vp8Header::setTl0PicIdx(payload, 10);
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        rtpHeader->sequenceNumber.get(),
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(4, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(4, rewrittenExtendedSequenceNumber);
    EXPECT_EQ(4, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(4, codec::Vp8Header::getTl0PicIdx(payload));
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());
}

TEST_F(Vp8RewriterTest, countersAreConsecutiveWhenSsrcIsChangedSequenceNumberLower)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    rtpHeader->ssrc = 1;
    rtpHeader->sequenceNumber = 1;
    rtpHeader->timestamp = 1;
    codec::Vp8Header::setPicId(payload, 1);
    codec::Vp8Header::setTl0PicIdx(payload, 1);
    uint32_t rewrittenExtendedSequenceNumber = 0;
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        rtpHeader->sequenceNumber.get(),
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(2, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(2, rewrittenExtendedSequenceNumber);
    EXPECT_EQ(2, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(2, codec::Vp8Header::getTl0PicIdx(payload));
    auto lastTimestamp = rtpHeader->timestamp.get();

    rtpHeader->ssrc = 1;
    rtpHeader->sequenceNumber = 2;
    rtpHeader->timestamp = 2;
    codec::Vp8Header::setPicId(payload, 2);
    codec::Vp8Header::setTl0PicIdx(payload, 2);
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        rtpHeader->sequenceNumber.get(),
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(3, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(3, rewrittenExtendedSequenceNumber);
    EXPECT_EQ(3, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(3, codec::Vp8Header::getTl0PicIdx(payload));
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());
    lastTimestamp = rtpHeader->timestamp.get();

    rtpHeader->ssrc = 2;
    rtpHeader->sequenceNumber = 65535;
    rtpHeader->timestamp = 10000;
    codec::Vp8Header::setPicId(payload, 10);
    codec::Vp8Header::setTl0PicIdx(payload, 10);
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        rtpHeader->sequenceNumber.get(),
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(4, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(4, rewrittenExtendedSequenceNumber);
    EXPECT_EQ(4, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(4, codec::Vp8Header::getTl0PicIdx(payload));
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());
}

TEST_F(Vp8RewriterTest, countersAreConsecutiveIfLastPacketBeforeSwitchIsReordered)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    rtpHeader->ssrc = 1;
    rtpHeader->sequenceNumber = 1;
    rtpHeader->timestamp = 1;
    codec::Vp8Header::setPicId(payload, 1);
    codec::Vp8Header::setTl0PicIdx(payload, 1);
    uint32_t rewrittenExtendedSequenceNumber = 0;
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        rtpHeader->sequenceNumber.get(),
        "",
        rewrittenExtendedSequenceNumber);

    rtpHeader->ssrc = 1;
    rtpHeader->sequenceNumber = 3;
    rtpHeader->timestamp = 3;
    codec::Vp8Header::setPicId(payload, 3);
    codec::Vp8Header::setTl0PicIdx(payload, 3);
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        rtpHeader->sequenceNumber.get(),
        "",
        rewrittenExtendedSequenceNumber);

    rtpHeader->ssrc = 1;
    rtpHeader->sequenceNumber = 2;
    rtpHeader->timestamp = 2;
    codec::Vp8Header::setPicId(payload, 2);
    codec::Vp8Header::setTl0PicIdx(payload, 2);
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        rtpHeader->sequenceNumber.get(),
        "",
        rewrittenExtendedSequenceNumber);

    rtpHeader->ssrc = 2;
    rtpHeader->sequenceNumber = 10;
    rtpHeader->timestamp = 10;
    codec::Vp8Header::setPicId(payload, 10);
    codec::Vp8Header::setTl0PicIdx(payload, 10);
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        rtpHeader->sequenceNumber.get(),
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(5, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(5, rewrittenExtendedSequenceNumber);
    EXPECT_EQ(5, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(5, codec::Vp8Header::getTl0PicIdx(payload));
}

TEST_F(Vp8RewriterTest, countersAreConsecutiveWhenSsrcIsUnchangedAndSequenceRollover)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    rtpHeader->ssrc = outboundSsrc;
    rtpHeader->sequenceNumber = 65535;
    uint32_t extendedSequenceNumber = rtpHeader->sequenceNumber.get();
    rtpHeader->timestamp = 1;
    codec::Vp8Header::setPicId(payload, 1);
    codec::Vp8Header::setTl0PicIdx(payload, 1);
    uint32_t rewrittenExtendedSequenceNumber = 0;
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        extendedSequenceNumber,
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(65535, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(extendedSequenceNumber, rewrittenExtendedSequenceNumber);
    EXPECT_EQ(1, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(1, codec::Vp8Header::getTl0PicIdx(payload));
    auto lastTimestamp = rtpHeader->timestamp.get();

    rtpHeader->ssrc = outboundSsrc;
    rtpHeader->sequenceNumber = 0;
    extendedSequenceNumber = rtpHeader->sequenceNumber.get() | (1 << 16);
    rtpHeader->timestamp = 2;
    codec::Vp8Header::setPicId(payload, 2);
    codec::Vp8Header::setTl0PicIdx(payload, 2);
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        extendedSequenceNumber,
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(0, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(extendedSequenceNumber, rewrittenExtendedSequenceNumber);
    EXPECT_EQ(2, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(2, codec::Vp8Header::getTl0PicIdx(payload));
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());
    lastTimestamp = rtpHeader->timestamp.get();

    rtpHeader->ssrc = outboundSsrc;
    rtpHeader->sequenceNumber = 1;
    extendedSequenceNumber = rtpHeader->sequenceNumber.get() | (1 << 16);
    rtpHeader->timestamp = 3;
    codec::Vp8Header::setPicId(payload, 3);
    codec::Vp8Header::setTl0PicIdx(payload, 3);
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        extendedSequenceNumber,
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(1, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(extendedSequenceNumber, rewrittenExtendedSequenceNumber);
    EXPECT_EQ(3, codec::Vp8Header::getPicId(payload));
    EXPECT_EQ(3, codec::Vp8Header::getTl0PicIdx(payload));
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());
}

TEST_F(Vp8RewriterTest, countersAreConsecutiveIfLastPacketBeforeSwitchIsReorderedWithRollover)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    rtpHeader->ssrc = outboundSsrc;
    rtpHeader->sequenceNumber = 65534;
    uint32_t extendedSequenceNumber = rtpHeader->sequenceNumber.get();
    rtpHeader->timestamp = 1;
    codec::Vp8Header::setPicId(payload, 1);
    codec::Vp8Header::setTl0PicIdx(payload, 1);
    uint32_t rewrittenExtendedSequenceNumber = 0;
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        extendedSequenceNumber,
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(65534, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(extendedSequenceNumber, rewrittenExtendedSequenceNumber);

    rtpHeader->ssrc = outboundSsrc;
    rtpHeader->sequenceNumber = 0;
    extendedSequenceNumber = rtpHeader->sequenceNumber.get() | (1 << 16);
    rtpHeader->timestamp = 3;
    codec::Vp8Header::setPicId(payload, 3);
    codec::Vp8Header::setTl0PicIdx(payload, 3);
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        extendedSequenceNumber,
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(0, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(extendedSequenceNumber, rewrittenExtendedSequenceNumber);

    rtpHeader->ssrc = outboundSsrc;
    rtpHeader->sequenceNumber = 65535;
    extendedSequenceNumber = rtpHeader->sequenceNumber.get();
    rtpHeader->timestamp = 2;
    codec::Vp8Header::setPicId(payload, 2);
    codec::Vp8Header::setTl0PicIdx(payload, 2);
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        extendedSequenceNumber,
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(65535, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(extendedSequenceNumber, rewrittenExtendedSequenceNumber);

    rtpHeader->ssrc = 2;
    rtpHeader->sequenceNumber = 4711;
    extendedSequenceNumber = rtpHeader->sequenceNumber.get() | (1 << 16);
    rtpHeader->timestamp = 10;
    codec::Vp8Header::setPicId(payload, 10);
    codec::Vp8Header::setTl0PicIdx(payload, 10);
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        extendedSequenceNumber,
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(1, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(1 | (1 << 16), rewrittenExtendedSequenceNumber);
}

TEST_F(Vp8RewriterTest, longGapInSequenceNumbersSameSsrc)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    rtpHeader->ssrc = outboundSsrc;
    rtpHeader->sequenceNumber = 10000;
    uint32_t extendedSequenceNumber = rtpHeader->sequenceNumber.get();
    rtpHeader->timestamp = 1;
    codec::Vp8Header::setPicId(payload, 1);
    codec::Vp8Header::setTl0PicIdx(payload, 1);
    uint32_t rewrittenExtendedSequenceNumber = 0;
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        extendedSequenceNumber,
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(10000, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(extendedSequenceNumber, rewrittenExtendedSequenceNumber);

    rtpHeader->ssrc = outboundSsrc;
    rtpHeader->sequenceNumber = 10;
    extendedSequenceNumber = rtpHeader->sequenceNumber.get() | (3 << 16);
    rtpHeader->timestamp = 1000;
    codec::Vp8Header::setPicId(payload, 3);
    codec::Vp8Header::setTl0PicIdx(payload, 3);
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        extendedSequenceNumber,
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(10, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(extendedSequenceNumber, rewrittenExtendedSequenceNumber);
}

TEST_F(Vp8RewriterTest, longGapInSequenceNumbersNewSsrc)
{
    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    rtpHeader->ssrc = outboundSsrc;
    rtpHeader->sequenceNumber = 10000;
    uint32_t extendedSequenceNumber = rtpHeader->sequenceNumber.get();
    rtpHeader->timestamp = 1;
    codec::Vp8Header::setPicId(payload, 1);
    codec::Vp8Header::setTl0PicIdx(payload, 1);
    uint32_t rewrittenExtendedSequenceNumber = 0;
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        extendedSequenceNumber,
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(10000, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(extendedSequenceNumber, rewrittenExtendedSequenceNumber);

    rtpHeader->ssrc = 2;
    rtpHeader->sequenceNumber = 30000;
    extendedSequenceNumber = rtpHeader->sequenceNumber.get() | (3 << 16);
    rtpHeader->timestamp = 1000;
    codec::Vp8Header::setPicId(payload, 3);
    codec::Vp8Header::setTl0PicIdx(payload, 3);
    bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
        *packet,
        outboundSsrc,
        extendedSequenceNumber,
        "",
        rewrittenExtendedSequenceNumber);

    EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
    EXPECT_EQ(10001, rtpHeader->sequenceNumber.get());
    EXPECT_EQ(10001, rewrittenExtendedSequenceNumber);
}

TEST_F(Vp8RewriterTest, countersAreConsecutiveWhenSsrcChangeAndRtx)
{
    memory::PacketPoolAllocator packetAllocator(512, "test");
    bridge::RtpMap map1(bridge::RtpMap::Format::VP8);
    bridge::SsrcOutboundContext outboundContext1(1, packetAllocator, map1);
    bridge::SsrcOutboundContext outboundContext2(2, packetAllocator, map1);

    auto packet = memory::makeUniquePacket(*_allocator);
    packet->setLength(packet->size);

    auto rtpHeader = rtp::RtpHeader::create(*packet);
    auto payload = rtpHeader->getPayload();
    std::array<uint8_t, 6> vp8PayloadDescriptor = {0x90, 0xe0, 0xab, 0xb9, 0xd3, 0x60};
    memcpy(payload, vp8PayloadDescriptor.data(), vp8PayloadDescriptor.size());

    uint32_t rewrittenExtendedSequenceNumber = 0;
    uint32_t lastTimestamp = 0;

    auto examine = [&](bridge::SsrcOutboundContext& outboundContext,
                       uint16_t seqNo,
                       uint32_t timestamp,
                       uint16_t expectedSeqNo,
                       uint16_t picId,
                       uint16_t picIdx,
                       uint16_t expectedPicId,
                       uint16_t expectedPicIdx) {
        rtpHeader->ssrc = outboundContext.ssrc;
        rtpHeader->sequenceNumber = seqNo;
        rtpHeader->timestamp = timestamp;
        codec::Vp8Header::setPicId(payload, picId);
        codec::Vp8Header::setTl0PicIdx(payload, picIdx);

        bridge::Vp8Rewriter::rewrite(*_ssrcOutboundContext,
            *packet,
            outboundSsrc,
            rtpHeader->sequenceNumber.get(),
            "",
            rewrittenExtendedSequenceNumber);

        uint16_t nextSequenceNumber;
        auto seqConversion = utils::OutboundSequenceNumber::process(rewrittenExtendedSequenceNumber,
            outboundContext.highestSeenExtendedSequenceNumber,
            outboundContext.sequenceCounter,
            nextSequenceNumber);
        EXPECT_TRUE(seqConversion);
        printf("seqno %u\n", seqNo);
        rtpHeader->sequenceNumber = nextSequenceNumber;

        EXPECT_EQ(outboundSsrc, rtpHeader->ssrc.get());
        EXPECT_EQ(expectedSeqNo, rtpHeader->sequenceNumber.get());
        EXPECT_EQ(expectedSeqNo, rewrittenExtendedSequenceNumber);
        EXPECT_EQ(expectedPicId, codec::Vp8Header::getPicId(payload));
        EXPECT_EQ(expectedPicIdx, codec::Vp8Header::getTl0PicIdx(payload));
    };

    examine(outboundContext1, 74, 60000, 75, 32764, 255, 32765, 0);
    examine(outboundContext1, 75, 60010, 76, 2, 2, 3, 3);

    // change ssrc
    lastTimestamp = rtpHeader->timestamp.get();
    examine(outboundContext2, 82, 10000, 77, 10, 10, 4, 4);
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());

    // emulate 2 rtx
    examine(outboundContext2, 3, 10000, 65534, 10, 10, 4, 4);
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());

    examine(outboundContext2, 4, 10000, 65535, 10, 10, 4, 4);
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());

    // continue rtp
    examine(outboundContext2, 83, 10000, 78, 10, 10, 4, 4);
    EXPECT_LT(lastTimestamp, rtpHeader->timestamp.get());
}
