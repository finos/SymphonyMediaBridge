#include "bwe/BandwidthEstimator.h"
#include "concurrency/MpmcQueue.h"
#include "logger/Logger.h"
#include "math/Matrix.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/JitterTracker.h"
#include "rtp/RtpHeader.h"
#include "test/bwe/FakeAudioSource.h"
#include "test/bwe/FakeCall.h"
#include "test/bwe/FakeCrossTraffic.h"
#include "test/bwe/FakeVideoSource.h"
#include "test/transport/NetworkLink.h"
#include <gtest/gtest.h>

using namespace math;

TEST(JitterTest, jitterTracker)
{
    rtp::JitterTracker tracker(48000);

    uint32_t rtpTimestamp = 500;
    uint64_t receiveTime = utils::Time::getAbsoluteTime();

    for (int i = 0; i < 8000; ++i)
    {
        tracker.update(receiveTime, rtpTimestamp);
        rtpTimestamp += 960; // 20ms
        receiveTime += (15 + rand() % 11) * utils::Time::ms;
    }
    logger::debug("jit %u", "", tracker.get());
    EXPECT_NEAR(tracker.get(), 25 * 48 / 10, 10);

    // receive with 2ms difference in interval compared to send interval
    for (int i = 0; i < 8000; ++i)
    {
        tracker.update(receiveTime, rtpTimestamp);
        rtpTimestamp += 960; // 20ms
        receiveTime += i & 1 ? 22 * utils::Time::ms : 18 * utils::Time::ms;
    }
    logger::debug("jit %u", "", tracker.get());
    EXPECT_NEAR(tracker.get(), 2 * 48, 15);
}
