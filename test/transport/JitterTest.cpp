#include "bwe/BandwidthEstimator.h"
#include "codec/AudioFader.h"
#include "concurrency/MpmcQueue.h"
#include "logger/Logger.h"
#include "logger/PacketLogger.h"
#include "math/Matrix.h"
#include "math/WelfordVariance.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/JitterBufferList.h"
#include "rtp/JitterEstimator.h"
#include "rtp/JitterTracker.h"
#include "rtp/RtpHeader.h"
#include "rtp/SendTimeDial.h"
#include "test/CsvWriter.h"
#include "test/bwe/FakeAudioSource.h"
#include "test/bwe/FakeCall.h"
#include "test/bwe/FakeCrossTraffic.h"
#include "test/bwe/FakeVideoSource.h"
#include "test/transport/NetworkLink.h"
#include <gtest/gtest.h>

using namespace math;

#include "utils/ScopedFileHandle.h"
#include <gtest/gtest.h>
#include <random>

using namespace math;

namespace rtp
{
template <typename T, size_t S>
class Backlog
{
public:
    Backlog() : _index(0) {}
    T add(T value)
    {
        _index = (_index + 1) % S;
        auto prev = _values[_index];
        _values[_index] = value;
        return prev;
    }

    T front() { return _values[_index]; }
    T back() { return _values[(_index + 1) % S]; }

    T getMean() const
    {
        T acc = 0;
        for (int i = 0; i < S; ++i)
        {
            acc += _values[i];
        }

        return acc / S;
    }

    T getVariance(T hypotheticalMean) const
    {
        T acc = 0;
        for (size_t i = 0; i < S; ++i)
        {
            auto d = _values[i] - hypotheticalMean;
            acc += d * d;
        }

        return acc / S;
    }

private:
    T _values[S];
    uint32_t _index;
};
} // namespace rtp
namespace
{
class PacketJitterEmulator
{
public:
    PacketJitterEmulator(memory::PacketPoolAllocator& allocator, uint32_t ssrc)
        : _source(allocator, 80, ssrc),
          _jitter(10),
          _link("Jitter", 2500, 15000, 1500)
    {
    }

    void setJitter(float ms) { _jitter = ms; }

    memory::UniquePacket get(uint64_t timestamp)
    {
        auto packet = _source.getPacket(timestamp);
        if (packet)
        {
            _link.push(std::move(packet), timestamp, false);
            if (_link.count() == 1)
            {
                uint32_t latency = (rand() % static_cast<uint32_t>(_jitter * 101)) / 100;
                _link.injectDelaySpike(latency);
            }
        }

        return _link.pop(timestamp);
    }

    uint64_t nextEmitTime(uint64_t timestamp) const { return _link.timeToRelease(timestamp); }

private:
    fakenet::FakeAudioSource _source;
    double _jitter;

    memory::UniquePacket _packet;
    fakenet::NetworkLink _link;
};
} // namespace

TEST(Welford, uniform)
{
    math::WelfordVariance<double> w1;
    math::RollingWelfordVariance<double> w2(250);
    uint32_t range = 1000;
    for (int i = 0; i < 2500; ++i)
    {
        double val = rand() % (range + 1);
        w1.add(val);
        w2.add(val);

        if (i % 100 == 0)
        {
            logger::debug("w1 %f, %f, w2 %f, %f",
                "",
                w1.getMean(),
                sqrt(w1.getVariance()),
                w2.getMean(),
                sqrt(w2.getVariance()));
        }
    }

    EXPECT_NEAR(w2.getMean(), 500.0, 20.0);
    EXPECT_NEAR(w2.getVariance(), (range * range) / 12, 1500);
}

TEST(Welford, normal)
{
    math::WelfordVariance<double> w1;
    math::RollingWelfordVariance<double> w2(250);

    std::random_device rd{};
    std::mt19937 gen{rd()};

    // values near the mean are the most likely
    // standard deviation affects the dispersion of generated values from the mean
    std::normal_distribution<> normalDistribution{500, 100};

    for (int i = 0; i < 2500; ++i)
    {
        double val = normalDistribution(gen);
        w1.add(val);
        w2.add(val);

        if (i % 100 == 0)
        {
            logger::debug("w1 %f, %f, w2 %f, %f",
                "",
                w1.getMean(),
                sqrt(w1.getVariance()),
                w2.getMean(),
                sqrt(w2.getVariance()));
        }
    }
    EXPECT_NEAR(w2.getMean(), 500.0, 25.0);
    EXPECT_NEAR(w2.getVariance(), 100.0 * 100, 2900.0);
}

class JitterBufferTest : public ::testing::Test
{
public:
    JitterBufferTest() : _allocator(400, "test") {}

    memory::PacketPoolAllocator _allocator;
    rtp::JitterBufferList _buffer;
};

// jitter below 10ms will never affect subsequent packet. It is just a delay inflicted on each packet unrelated to
// previous events
TEST(JitterTest, a10ms)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);
    rtp::JitterEstimator jitt(48000);
    emulator.setJitter(10);

    uint64_t timestamp = utils::Time::getAbsoluteTime();

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 15000; ++i)
    {
        auto p = emulator.get(timestamp);
        if (p)
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(timestamp, header->timestamp.get());

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
    }

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 600);
    EXPECT_GE(countBelow, 600);
    EXPECT_GE(below95, 1440);
    EXPECT_NEAR(jitt.getJitter(), 5.5, 1.5);
}

// delay above 20ms will affect subsequent packet as previous packet has not been delivered yet.
TEST(JitterTest, a30ms)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);
    emulator.setJitter(30);
    rtp::JitterEstimator jitt(48000);
    rtp::Backlog<double, 1500> blog;
    math::RollingWelfordVariance<double> var2(250);
    uint64_t timestamp = utils::Time::getAbsoluteTime();

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 15000; ++i)
    {
        auto p = emulator.get(timestamp);
        if (p)
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(timestamp, header->timestamp.get());
            blog.add(delay);
            var2.add(delay);

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
    }

    auto m = jitt.getJitter();
    for (int i = 0; i < 10; ++i)
    {
        auto a = m - 2.0 + i * 0.4;
        logger::info("m %.4f  std %.4f", "", a, blog.getVariance(a));
    }
    logger::info("roll wf m %.4f  std %.4f", "", var2.getMean(), blog.getVariance(var2.getMean()));

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 600);
    EXPECT_GE(countBelow, 600);
    EXPECT_GE(below95, 1440);
    EXPECT_NEAR(jitt.getJitter(), 12.0, 1.5);
}

TEST(JitterTest, clockSkewRxFaster)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);
    rtp::JitterEstimator jitt(48000);

    uint64_t timestamp = utils::Time::getAbsoluteTime();
    uint64_t remoteTimestamp = timestamp;

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 15000; ++i)
    {
        auto p = emulator.get(timestamp);
        if (p)
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(remoteTimestamp, header->timestamp.get());

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
        remoteTimestamp += utils::Time::ms * 2;
        // 500 iterations per s
        if (i % 5 == 0) // 0.01s
        {
            remoteTimestamp += utils::Time::us * 5; // 0.5ms / s
        }
    }

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 650);
    EXPECT_GE(countBelow, 650);
    EXPECT_GE(below95, 1440);
    EXPECT_NEAR(jitt.getJitter(), 5.5, 1.5);
}

TEST(JitterTest, clockSkewRxSlower)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);
    rtp::JitterEstimator jitt(48000);

    uint64_t timestamp = utils::Time::getAbsoluteTime();
    uint64_t remoteTimestamp = timestamp;

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 15000; ++i)
    {
        auto p = emulator.get(timestamp);
        if (p)
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(remoteTimestamp, header->timestamp.get());

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
        remoteTimestamp += utils::Time::ms * 2;
        if (i % 5 == 0) // 0.01s
        {
            timestamp += utils::Time::us * 5;
        }
    }

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 700);
    EXPECT_GE(countBelow, 700);
    EXPECT_GE(below95, 1440);
    EXPECT_NEAR(jitt.getJitter(), 5.5, 1.5);
}

TEST(JitterTest, a230)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);
    emulator.setJitter(230);
    rtp::JitterEstimator jitt(48000);

    uint64_t timestamp = utils::Time::getAbsoluteTime();

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 15000; ++i)
    {
        auto p = emulator.get(timestamp);
        if (p)
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(timestamp, header->timestamp.get());

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
    }

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 600);
    EXPECT_GE(countBelow, 600);
    EXPECT_GE(below95, 1400);
    EXPECT_NEAR(jitt.getJitter(), 75, 15.5);
}

TEST(JitterTest, gap)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);
    rtp::JitterEstimator jitt(48000);

    uint64_t timestamp = utils::Time::getAbsoluteTime();

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 15000; ++i)
    {
        auto p = emulator.get(timestamp);
        if (p && (i < 300 || i > 450))
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(timestamp, header->timestamp.get());

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
    }

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 600);
    EXPECT_GE(countBelow, 600);
    EXPECT_GE(below95, 1440);
    EXPECT_NEAR(jitt.getJitter(), 5.5, 1.5);
}

TEST(JitterTest, dtx)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    rtp::RtpDelayTracker delayTracker(48000);

    uint64_t receiveTime = 5000;
    uint32_t rtpTimestamp = 7800;
    for (int i = 0; i < 300; ++i)
    {
        delayTracker.update(receiveTime + (rand() % 40) * utils::Time::ms / 10, rtpTimestamp);
        receiveTime += utils::Time::ms * 20;
        rtpTimestamp += 960;
    }

    receiveTime += utils::Time::minute * 3;
    rtpTimestamp += 24 * 60 * 48000; // 24 min increase in rtptimestamp
    auto delay = delayTracker.update(receiveTime, rtpTimestamp);
    EXPECT_EQ(delay, 0);

    rtpTimestamp += 960;
    receiveTime += utils::Time::ms * 20;
    delay = delayTracker.update(receiveTime + 2 * utils::Time::ms, rtpTimestamp);
    EXPECT_NEAR(delay, 2.0 * utils::Time::ms, 11 * utils::Time::us);

    rtpTimestamp -= 3 * 48000; // rollback rtp by 3s, would look like huge 3s jitter
    receiveTime += utils::Time::ms * 20;
    delay = delayTracker.update(receiveTime, rtpTimestamp);
    EXPECT_LE(delay, utils::Time::ms * 10);

    rtpTimestamp += 960;
    receiveTime += utils::Time::ms * 200;
    delay = delayTracker.update(receiveTime, rtpTimestamp);
    EXPECT_GE(delay, utils::Time::ms * 179);

    // run for 10s
    for (int i = 0; i < 9 * 50; ++i)
    {
        rtpTimestamp += 960;
        receiveTime += utils::Time::ms * 20;
        delay = delayTracker.update(receiveTime + (rand() % 4) * utils::Time::ms, rtpTimestamp);
    }
    EXPECT_LT(delay, utils::Time::ms * 9);

    receiveTime += utils::Time::sec * 10;
    for (int i = 0; i < 10 * 50; ++i)
    {
        rtpTimestamp += 960;
        receiveTime += utils::Time::ms * 20;
        delay = delayTracker.update(receiveTime + (rand() % 4) * utils::Time::ms, rtpTimestamp);
    }
    EXPECT_LT(delay, utils::Time::ms * 100);
}

TEST(JitterTest, adaptDown23)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);
    emulator.setJitter(200);
    rtp::JitterEstimator jitt(48000);

    uint64_t timestamp = utils::Time::getAbsoluteTime();

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 15000; ++i)
    {
        if (i == 9980)
        {
            EXPECT_NEAR(jitt.getJitter(), 72, 14.0);
            emulator.setJitter(23);
            logger::info("decrease jitter to 23ms", "");
        }

        auto p = emulator.get(timestamp);
        if (p)
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(timestamp, header->timestamp.get());

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
    }

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 500);
    EXPECT_GE(countBelow, 500);
    EXPECT_GE(below95, 1440);
    EXPECT_NEAR(jitt.getJitter(), 10, 1.5);
}

TEST(JitterTest, adaptUp200)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);

    rtp::JitterEstimator jitt(48000);

    uint64_t timestamp = utils::Time::getAbsoluteTime();
    rtp::JitterTracker tracker(48000);

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 55000; ++i)
    {
        if (i == 9980)
        {
            EXPECT_NEAR(jitt.getJitter(), 4.5, 1.5);
            emulator.setJitter(200);
            logger::info("incr jitter to 30ms", "");
        }

        auto p = emulator.get(timestamp);
        if (p)
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(timestamp, header->timestamp.get());
            tracker.update(timestamp, header->timestamp.get());

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
        if (i % 500 == 0)
        {
            logger::info("jitter %f, 95p %f, maxJ %f, ietf jitter %fms",
                "JitterEstimator",
                jitt.getJitter(),
                jitt.get95Percentile(),
                jitt.getMaxJitter(),
                tracker.get() / 48.0);
        }
    }

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 600);
    EXPECT_GE(countBelow, 600);
    EXPECT_GE(below95, 1430);
    EXPECT_NEAR(jitt.getJitter(), 75, 13.5);
}

TEST(JitterTest, adaptUp30)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);
    emulator.setJitter(10);
    rtp::JitterEstimator jitt(48000);

    uint64_t timestamp = utils::Time::getAbsoluteTime();
    rtp::JitterTracker tracker(48000);

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 12500; ++i)
    {
        if (i == 9980)
        {
            EXPECT_NEAR(jitt.getJitter(), 4.5, 1.5);
            emulator.setJitter(30);
            logger::info("incr jitter to 30ms", "");
        }

        auto p = emulator.get(timestamp);
        if (p)
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(timestamp, header->timestamp.get());
            tracker.update(timestamp, header->timestamp.get());

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
        if (i % 500 == 0)
        {
            logger::info("jitter %f, 95p %f, maxJ %f, ietf jitter %fms",
                "JitterEstimator",
                jitt.getJitter(),
                jitt.get95Percentile(),
                jitt.getMaxJitter(),
                tracker.get() / 48.0);
        }
    }

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 600);
    EXPECT_GE(countBelow, 600);
    EXPECT_GE(below95, 1150);
    EXPECT_NEAR(jitt.getJitter(), 12, 2.5);
}

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
    EXPECT_NEAR(tracker.get(), 25 * 48 / 10, 5);

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

TEST_F(JitterBufferTest, bufferPlain)
{
    memory::Packet stageArea;
    {
        auto header = rtp::RtpHeader::create(stageArea);
        header->ssrc = 4000;
        stageArea.setLength(250);
    }
    const auto oneFromFull = _buffer.SIZE - 2;

    for (int i = 0; i < oneFromFull; ++i)
    {
        auto p = memory::makeUniquePacket(_allocator, stageArea);
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = i + 100;
        header->timestamp = 56000 + i * 960;

        EXPECT_TRUE(_buffer.add(std::move(p)));
    }

    EXPECT_EQ(_buffer.count(), oneFromFull);
    EXPECT_EQ(_buffer.getRtpDelay(), 960 * (oneFromFull - 1));
    EXPECT_FALSE(_buffer.empty());
    EXPECT_EQ(_buffer.getFrontRtp()->timestamp.get(), 56000);

    // make a gap of 3 pkts
    auto p = memory::makeUniquePacket(_allocator, stageArea);
    {
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = 100 + oneFromFull;
        header->timestamp = 56000 + oneFromFull * 960;
    }
    EXPECT_TRUE(_buffer.add(std::move(p)));
    EXPECT_EQ(_buffer.getRtpDelay(), 960 * oneFromFull);

    uint16_t prevSeq = 99;
    uint32_t count = 0;
    EXPECT_EQ(_buffer.count(), oneFromFull + 1);
    for (auto packet = _buffer.pop(); packet; packet = _buffer.pop())
    {
        auto header = rtp::RtpHeader::fromPacket(*packet);
        EXPECT_GT(header->sequenceNumber.get(), prevSeq);
        prevSeq = header->sequenceNumber.get();
        ++count;
    }
    EXPECT_EQ(count, oneFromFull + 1);
}

TEST_F(JitterBufferTest, bufferReorder)
{
    memory::Packet stageArea;
    {
        auto header = rtp::RtpHeader::create(stageArea);
        header->ssrc = 4000;
        stageArea.setLength(250);
    }

    for (int i = 0; i < 5; ++i)
    {
        auto p = memory::makeUniquePacket(_allocator, stageArea);
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = i + 100;
        header->timestamp = 56000 + i * 960;

        EXPECT_TRUE(_buffer.add(std::move(p)));
    }

    EXPECT_EQ(_buffer.count(), 5);
    EXPECT_EQ(_buffer.getRtpDelay(), 960 * 4);
    EXPECT_FALSE(_buffer.empty());
    EXPECT_EQ(_buffer.getFrontRtp()->timestamp.get(), 56000);

    // make a gap
    auto p = memory::makeUniquePacket(_allocator, stageArea);
    {
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = 110;
        header->timestamp = 56000 + 10 * 960;
    }
    EXPECT_TRUE(_buffer.add(std::move(p)));
    EXPECT_EQ(_buffer.getRtpDelay(), 960 * 10);

    // reorder
    p = memory::makeUniquePacket(_allocator, stageArea);
    {
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = 105;
        header->timestamp = 56000 + 5 * 960;
    }
    EXPECT_TRUE(_buffer.add(std::move(p)));
    EXPECT_EQ(_buffer.getRtpDelay(), 960 * 10);
    EXPECT_EQ(_buffer.count(), 7);

    p = memory::makeUniquePacket(_allocator, stageArea);
    {
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = 108;
        header->timestamp = 56000 + 28 * 960;
    }
    EXPECT_TRUE(_buffer.add(std::move(p)));

    uint16_t prevSeq = 99;
    uint32_t count = 0;
    for (auto packet = _buffer.pop(); packet; packet = _buffer.pop())
    {
        auto header = rtp::RtpHeader::fromPacket(*packet);
        EXPECT_GT(header->sequenceNumber.get(), prevSeq);
        prevSeq = header->sequenceNumber.get();
        ++count;
    }
    EXPECT_EQ(count, 8);
}

TEST_F(JitterBufferTest, bufferEmptyFull)
{
    memory::Packet stageArea;
    {
        auto header = rtp::RtpHeader::create(stageArea);
        header->ssrc = 4000;
        stageArea.setLength(250);
    }

    EXPECT_EQ(_buffer.pop(), nullptr);

    for (int i = 0; i < _buffer.SIZE; ++i)
    {
        auto p = memory::makeUniquePacket(_allocator, stageArea);
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = i + 100;
        header->timestamp = 56000 + i * 960;

        EXPECT_TRUE(_buffer.add(std::move(p)));
    }

    EXPECT_EQ(_buffer.count(), _buffer.SIZE);
    EXPECT_EQ(_buffer.getRtpDelay(), 960 * (_buffer.SIZE - 1));
    EXPECT_FALSE(_buffer.empty());
    EXPECT_EQ(_buffer.getFrontRtp()->timestamp.get(), 56000);

    auto p = memory::makeUniquePacket(_allocator, stageArea);
    {
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = 100 + _buffer.count();
        header->timestamp = 56000 + 10 * 960;
    }
    EXPECT_FALSE(_buffer.add(std::move(p)));
    EXPECT_EQ(_buffer.getRtpDelay(), 960 * (_buffer.SIZE - 1));

    uint16_t prevSeq = 99;
    uint32_t count = 0;
    EXPECT_EQ(_buffer.count(), _buffer.SIZE);
    for (auto packet = _buffer.pop(); packet; packet = _buffer.pop())
    {
        auto header = rtp::RtpHeader::fromPacket(*packet);
        EXPECT_GT(header->sequenceNumber.get(), prevSeq);
        prevSeq = header->sequenceNumber.get();
        ++count;
    }
    EXPECT_EQ(count, _buffer.SIZE);
    EXPECT_TRUE(_buffer.empty());
    EXPECT_EQ(_buffer.pop(), nullptr);
}

TEST_F(JitterBufferTest, reorderedFull)
{
    memory::Packet stageArea;
    {
        auto header = rtp::RtpHeader::create(stageArea);
        header->ssrc = 4000;
        stageArea.setLength(250);
    }

    EXPECT_EQ(_buffer.pop(), nullptr);

    for (int i = 0; i < _buffer.SIZE - 50; ++i)
    {
        auto p = memory::makeUniquePacket(_allocator, stageArea);
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = i + 100;
        header->timestamp = 56000 + i * 960;

        EXPECT_TRUE(_buffer.add(std::move(p)));
    }

    auto p = memory::makeUniquePacket(_allocator, stageArea);
    auto header = rtp::RtpHeader::create(*p);
    header->sequenceNumber = 49;
    header->timestamp = 56100;

    EXPECT_EQ(_buffer.count(), _buffer.SIZE - 50);
    EXPECT_TRUE(_buffer.add(std::move(p)));
}

TEST_F(JitterBufferTest, reorderedOne)
{
    memory::Packet stageArea;
    {
        auto header = rtp::RtpHeader::create(stageArea);
        header->ssrc = 4000;
        stageArea.setLength(250);
    }

    EXPECT_EQ(_buffer.pop(), nullptr);

    auto p = memory::makeUniquePacket(_allocator, stageArea);
    auto header = rtp::RtpHeader::create(*p);
    header->sequenceNumber = 100;
    header->timestamp = 56000;
    EXPECT_TRUE(_buffer.add(std::move(p)));

    p = memory::makeUniquePacket(_allocator, stageArea);
    header = rtp::RtpHeader::create(*p);
    header->sequenceNumber = 98;
    header->timestamp = 57000;
    EXPECT_TRUE(_buffer.add(std::move(p)));

    p = _buffer.pop();
    header = rtp::RtpHeader::fromPacket(*p);
    EXPECT_EQ(header->sequenceNumber.get(), 98);

    p = _buffer.pop();
    header = rtp::RtpHeader::fromPacket(*p);
    EXPECT_EQ(header->sequenceNumber.get(), 100);
}
