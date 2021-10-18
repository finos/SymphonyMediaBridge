#include "transport/recp/RecStreamAddedEventBuilder.h"
#include "crypto/SslHelper.h"
#include <gtest/gtest.h>

using namespace recp;

TEST(RecStreamAddedEventBuilderTest, buildEmptyPacket)
{
    memory::PacketPoolAllocator allocator(4096 * 32, "testMain");
    auto packet = RecStreamAddedEventBuilder(allocator).build();

    EXPECT_STREQ(crypto::toHexString(packet->get(), packet->getLength()).c_str(), "00020000000000000000000000000000");

    allocator.free(packet);
}

TEST(RecStreamAddedEventBuilderTest, setWallClockAfterEndpoint)
{
    const std::string endpointId = "endpoint-id-t";
    const size_t expectedPaddingBytes = 3;
    std::chrono::duration<long, std::milli> dur(0x1E1C6450DD3);
    std::chrono::system_clock::time_point wallClock(dur);
    const std::string expectedWallClockNtpValue = "ff001133aac08312";
    memory::PacketPoolAllocator allocator(4096 * 32, "testMain");
    auto packet = RecStreamAddedEventBuilder(allocator)
                      .setSequenceNumber(0x12B2)
                      .setTimestamp(0xFFAA1122)
                      .setSsrc(0x11224400)
                      .setIsScreenSharing(true)
                      .setRtpPayloadType(0x70)
                      .setBridgeCodecNumber(0xBB)
                      .setEndpoint(endpointId)
                      .setWallClock(wallClock)
                      .build();

    std::string sb;
    sb.reserve(100);

    sb.append("00") // ID
        .append("02") // Event type (Stream added)
        .append("12b2") // Sequence
        .append("ffaa1122") // timestamp
        .append("11224400") // ssrc
        .append("f0") // Screen share flag + RTP payload type
        .append("bb") // Bridge codec number
        .append("000d") // Endpoint id size
        .append(crypto::toHexString(endpointId.c_str(), endpointId.size())) // endpoint value
        .append(std::string(expectedPaddingBytes * 2, '0')) // padding
        .append(expectedWallClockNtpValue); // padding

    EXPECT_STREQ(crypto::toHexString(packet->get(), packet->getLength()).c_str(), sb.c_str());

    allocator.free(packet);
}

TEST(RecStreamAddedEventBuilderTest, setWallClockBeforeEndpointWithPadding)
{
    const std::string endpointId = "endpoint-id-t";
    const size_t expectedPaddingBytes = 3;
    std::chrono::duration<long, std::milli> dur(0x1E1C6450DD3);
    std::chrono::system_clock::time_point wallClock(dur);
    const std::string expectedWallClockNtpValue = "ff001133aac08312";
    memory::PacketPoolAllocator allocator(4096 * 32, "testMain");
    auto packet = RecStreamAddedEventBuilder(allocator)
                      .setSequenceNumber(0x12B2)
                      .setTimestamp(0xFFAA1122)
                      .setSsrc(0x11224400)
                      .setIsScreenSharing(false)
                      .setRtpPayloadType(0x70)
                      .setBridgeCodecNumber(0xBB)
                      .setWallClock(wallClock)
                      .setEndpoint(endpointId)
                      .build();

    std::string sb;
    sb.reserve(100);

    sb.append("00") // ID
        .append("02") // Event type (Stream added)
        .append("12b2") // Sequence
        .append("ffaa1122") // timestamp
        .append("11224400") // ssrc
        .append("70") // Screen share flag + RTP payload type
        .append("bb") // Bridge codec number
        .append("000d") // Endpoint id size
        .append(crypto::toHexString(endpointId.c_str(), endpointId.size())) // endpoint value
        .append(std::string(expectedPaddingBytes * 2, '0')) // padding
        .append(expectedWallClockNtpValue); // padding

    EXPECT_STREQ(crypto::toHexString(packet->get(), packet->getLength()).c_str(), sb.c_str());

    allocator.free(packet);
}

TEST(RecStreamAddedEventBuilderTest, setWallClockBeforeEndpointWithoutPadding)
{
    const std::string endpointId = "endpoint-id-test";
    std::chrono::duration<long, std::milli> dur(0x1E1C6450DD3);
    std::chrono::system_clock::time_point wallClock(dur);
    const std::string expectedWallClockNtpValue = "ff001133aac08312";
    memory::PacketPoolAllocator allocator(4096 * 32, "testMain");
    auto packet = RecStreamAddedEventBuilder(allocator)
                      .setSequenceNumber(0x12B2)
                      .setTimestamp(0xFFAA1122)
                      .setSsrc(0x11224400)
                      .setIsScreenSharing(true)
                      .setRtpPayloadType(0x70)
                      .setBridgeCodecNumber(0xBB)
                      .setWallClock(wallClock)
                      .setEndpoint(endpointId)
                      .build();

    std::string sb;
    sb.reserve(100);

    sb.append("00") // ID
        .append("02") // Event type (Stream added)
        .append("12b2") // Sequence
        .append("ffaa1122") // timestamp
        .append("11224400") // ssrc
        .append("f0") // Screen share flag + RTP payload type
        .append("bb") // Bridge codec number
        .append("0010") // Endpoint id size
        .append(crypto::toHexString(endpointId.c_str(), endpointId.size())) // endpoint value
        .append(expectedWallClockNtpValue); // padding

    EXPECT_STREQ(crypto::toHexString(packet->get(), packet->getLength()).c_str(), sb.c_str());

    allocator.free(packet);
}
