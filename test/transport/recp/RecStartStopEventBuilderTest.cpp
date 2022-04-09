#include "transport/recp/RecStartStopEventBuilder.h"
#include "crypto/SslHelper.h"
#include <gtest/gtest.h>

using namespace recp;

TEST(RecStartStopEventBuilder, setRecordingAndBeforeUserId)
{
    memory::PacketPoolAllocator allocator(4096 * 32, "testMain");
    auto packet = RecStartStopEventBuilder(allocator)
                      .setSequenceNumber(1)
                      .setTimestamp(1)
                      .setAudioEnabled(true)
                      .setVideoEnabled(true)
                      .setRecordingId("ABCDEFGHIJ")
                      .setUserId("KLMNOPQRST")
                      .build();

    EXPECT_STREQ(crypto::toHexString(packet->get(), packet->getLength()).c_str(),
        "0001000100000001030a0a004142434445464748494a4b4c4d4e4f5051525354");
}
TEST(RecStartStopEventBuilder, setUserIdBeforeRecordingId)
{
    memory::PacketPoolAllocator allocator(4096 * 32, "testMain");
    auto packet = RecStartStopEventBuilder(allocator)
                      .setSequenceNumber(1)
                      .setTimestamp(1)
                      .setAudioEnabled(true)
                      .setVideoEnabled(true)
                      .setUserId("KLMNOPQRST")
                      .setRecordingId("ABCDEFGHIJ")
                      .build();

    EXPECT_STREQ(crypto::toHexString(packet->get(), packet->getLength()).c_str(),
        "0001000100000001030a0a004142434445464748494a4b4c4d4e4f5051525354");
}
