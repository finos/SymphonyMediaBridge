#include "bwe/BandwidthEstimator.h"
#include "codec/AudioFader.h"
#include "concurrency/MpmcQueue.h"
#include "logger/Logger.h"
#include "logger/PacketLogger.h"
#include "math/Matrix.h"
#include "math/WelfordVariance.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/JitterBuffer.h"
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
    EXPECT_NEAR(w2.getMean(), 500.0, 10.0);
    EXPECT_NEAR(w2.getVariance(), 100.0 * 100, 2500.0);
}

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
            EXPECT_NEAR(jitt.getJitter(), 72, 12.5);
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
    EXPECT_NEAR(jitt.getJitter(), 75, 5.5);
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

TEST(JitterTest, bufferPlain)
{
    memory::PacketPoolAllocator allocator(200, "test");
    rtp::JitterBuffer buffer(27);

    memory::Packet stageArea;
    {
        auto header = rtp::RtpHeader::create(stageArea);
        header->ssrc = 4000;
        stageArea.setLength(250);
    }

    for (int i = 0; i < 25; ++i)
    {
        auto p = memory::makeUniquePacket(allocator, stageArea);
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = i + 100;
        header->timestamp = 56000 + i * 960;

        EXPECT_TRUE(buffer.add(std::move(p)));
    }

    EXPECT_EQ(buffer.count(), 25);
    EXPECT_EQ(buffer.getRtpDelay(), 960 * 24);
    EXPECT_FALSE(buffer.empty());
    EXPECT_EQ(buffer.getFrontRtp()->timestamp.get(), 56000);

    // make a gap of 3 pkts
    auto p = memory::makeUniquePacket(allocator, stageArea);
    {
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = 127;
        header->timestamp = 56000 + 27 * 960;
    }
    EXPECT_TRUE(buffer.add(std::move(p)));
    EXPECT_EQ(buffer.getRtpDelay(), 960 * 27);

    uint16_t prevSeq = 99;
    uint32_t count = 0;
    for (auto packet = buffer.pop(); packet; packet = buffer.pop())
    {
        auto header = rtp::RtpHeader::fromPacket(*packet);
        EXPECT_GT(header->sequenceNumber.get(), prevSeq);
        prevSeq = header->sequenceNumber.get();
        ++count;
    }
    EXPECT_EQ(count, 26);
}

TEST(JitterTest, bufferReorder)
{
    memory::PacketPoolAllocator allocator(200, "test");
    rtp::JitterBuffer buffer(27);

    memory::Packet stageArea;
    {
        auto header = rtp::RtpHeader::create(stageArea);
        header->ssrc = 4000;
        stageArea.setLength(250);
    }

    for (int i = 0; i < 5; ++i)
    {
        auto p = memory::makeUniquePacket(allocator, stageArea);
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = i + 100;
        header->timestamp = 56000 + i * 960;

        EXPECT_TRUE(buffer.add(std::move(p)));
    }

    EXPECT_EQ(buffer.count(), 5);
    EXPECT_EQ(buffer.getRtpDelay(), 960 * 4);
    EXPECT_FALSE(buffer.empty());
    EXPECT_EQ(buffer.getFrontRtp()->timestamp.get(), 56000);

    // make a gap
    auto p = memory::makeUniquePacket(allocator, stageArea);
    {
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = 110;
        header->timestamp = 56000 + 10 * 960;
    }
    EXPECT_TRUE(buffer.add(std::move(p)));
    EXPECT_EQ(buffer.getRtpDelay(), 960 * 10);

    // reorder
    p = memory::makeUniquePacket(allocator, stageArea);
    {
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = 105;
        header->timestamp = 56000 + 5 * 960;
    }
    EXPECT_TRUE(buffer.add(std::move(p)));
    EXPECT_EQ(buffer.getRtpDelay(), 960 * 10);
    EXPECT_EQ(buffer.count(), 7);

    p = memory::makeUniquePacket(allocator, stageArea);
    {
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = 108;
        header->timestamp = 56000 + 28 * 960;
    }
    EXPECT_TRUE(buffer.add(std::move(p)));

    uint16_t prevSeq = 99;
    uint32_t count = 0;
    for (auto packet = buffer.pop(); packet; packet = buffer.pop())
    {
        auto header = rtp::RtpHeader::fromPacket(*packet);
        EXPECT_GT(header->sequenceNumber.get(), prevSeq);
        prevSeq = header->sequenceNumber.get();
        ++count;
    }
    EXPECT_EQ(count, 8);
}

TEST(JitterTest, bufferEmptyFull)
{
    memory::PacketPoolAllocator allocator(200, "test");
    rtp::JitterBuffer buffer(27);

    memory::Packet stageArea;
    {
        auto header = rtp::RtpHeader::create(stageArea);
        header->ssrc = 4000;
        stageArea.setLength(250);
    }

    EXPECT_EQ(buffer.pop(), nullptr);

    for (int i = 0; i < 27; ++i)
    {
        auto p = memory::makeUniquePacket(allocator, stageArea);
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = i + 100;
        header->timestamp = 56000 + i * 960;

        EXPECT_TRUE(buffer.add(std::move(p)));
    }

    EXPECT_EQ(buffer.count(), 27);
    EXPECT_EQ(buffer.getRtpDelay(), 960 * 26);
    EXPECT_FALSE(buffer.empty());
    EXPECT_EQ(buffer.getFrontRtp()->timestamp.get(), 56000);

    auto p = memory::makeUniquePacket(allocator, stageArea);
    {
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = 110;
        header->timestamp = 56000 + 10 * 960;
    }
    EXPECT_FALSE(buffer.add(std::move(p)));
    EXPECT_EQ(buffer.getRtpDelay(), 960 * 26);

    uint16_t prevSeq = 99;
    uint32_t count = 0;
    for (auto packet = buffer.pop(); packet; packet = buffer.pop())
    {
        auto header = rtp::RtpHeader::fromPacket(*packet);
        EXPECT_GT(header->sequenceNumber.get(), prevSeq);
        prevSeq = header->sequenceNumber.get();
        ++count;
    }
    EXPECT_EQ(count, 27);
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.pop(), nullptr);
}

namespace
{
struct SsrcTrack
{
    uint64_t prevReceiveTime;
    double avgReceiveTime;
    uint32_t count;
};

uint32_t identifyAudioSsrc(logger::PacketLogReader& reader)
{
    logger::PacketLogItem item;
    std::map<uint32_t, SsrcTrack> ssrcs;
    for (int i = 0; reader.getNext(item); ++i)
    {
        if (item.size >= 300)
        {
            if (ssrcs.end() != ssrcs.find(item.ssrc))
            {
                ssrcs.erase(item.ssrc);
            }
            continue;
        }

        auto it = ssrcs.find(item.ssrc);
        if (ssrcs.end() == it)
        {
            ssrcs[item.ssrc] = SsrcTrack{item.receiveTimestamp, 0.02, 1};
            continue;
        }

        if (item.receiveTimestamp - it->second.prevReceiveTime > utils::Time::ms * 15)
        {
            it->second.prevReceiveTime = item.receiveTimestamp;
            ++it->second.count;

            if (it->second.count > 300)
            {
                return item.ssrc;
            }
        }
    }

    if (ssrcs.size() > 0)
    {
        return ssrcs.begin()->first;
    }
    return 0;
}

size_t eliminateSamples(int16_t* buf, size_t sampleCount, int everyNth)
{
    int16_t* endPtr = buf + sampleCount;
    int16_t* src = buf;
    size_t newSize = 0;
    for (int i = 1; src < endPtr; ++i)
    {
        *buf = *src;
        ++newSize;
        if (i % everyNth == 0)
        {
            ++src;
        }
        ++src;
        ++buf;
    }

    return newSize;
}

size_t eliminateSilence(int16_t* buf, size_t sampleCount)
{
    int16_t r[sampleCount];
    std::memset(r, 0, sampleCount * 2);
    int cursor = 0;
    for (size_t j = 0; j < sampleCount; ++j)
    {
        for (size_t i = 0; i < sampleCount; ++i)
        {
            if (std::abs(buf[j]) > 50)
            {
                r[cursor] = buf[j];
            }
        }
    }

    return cursor;
}

} // namespace

TEST(JitterRerun, file)
{
    uint16_t* tmpZeroes = new uint16_t[960 * 50];
    std::fill(tmpZeroes, tmpZeroes + 960 * 10, 0);
    std::array<std::string, 1> traces = {"Transport-6-4G-1-5Mbps"};
    for (const auto& trace : traces)
    {
        if (trace.empty())
        {
            break;
        }

        logger::info("scanning file %s", "", trace.c_str());

        utils::ScopedFileHandle audioFile(::fopen("./_bwelogs/2minrecording.raw", "r"));
        utils::ScopedFileHandle audioPlayback(::fopen("/mnt/c/dev/rtc/2minplayback.raw", "w+"));
        CsvWriter csv("./_ssdata/jdelay.csv");
        csv.writeLine("time, jbuf, delay");

        logger::PacketLogReader reader(::fopen(("./_bwelogs/" + trace).c_str(), "r"));
        uint32_t audioSsrc = identifyAudioSsrc(reader);
        reader.rewind();

        rtp::JitterTracker tracker(48000);
        math::RollingWelfordVariance<uint64_t> welf(75);
        rtp::RtpDelayTracker jitt(48000);
        logger::PacketLogItem item;
        uint64_t start = 0;
        uint32_t rtpTimestamp = 0;
        uint32_t prevSeqNo = 0;

        uint64_t prevRecv = 0;
        int32_t consumedSamples = 0;
        int32_t jitterBufferLevel = 0;

        for (int i = 0; reader.getNext(item); ++i)
        {

            if (item.ssrc == audioSsrc)
            {
                int16_t audioBuf[960];
                int readBytes = ::fread(audioBuf, 1, 960 * 2, audioFile.get());
                if (readBytes < 2 * 960)
                {
                    return;
                }
                if (prevSeqNo == 0)
                {
                    rtpTimestamp = 1000;
                    prevSeqNo = item.sequenceNumber;
                    start = item.receiveTimestamp;
                    prevRecv = item.receiveTimestamp - utils::Time::ms * 20;
                    jitterBufferLevel = 960;
                    consumedSamples = 0;
                    ::fwrite(audioBuf, 1, 960 * 2, audioPlayback.get());
                }
                else
                {
                    consumedSamples = (item.receiveTimestamp - prevRecv) * 48 / utils::Time::ms;
                    rtpTimestamp += 960 * (item.sequenceNumber - prevSeqNo);
                    prevSeqNo = item.sequenceNumber;
                    prevRecv = item.receiveTimestamp;

                    if (consumedSamples > jitterBufferLevel)
                    {
                        ::fwrite(tmpZeroes, (consumedSamples - jitterBufferLevel) * 2, 1, audioPlayback.get());
                        logger::debug("padding %d", "", (consumedSamples - jitterBufferLevel));
                        jitterBufferLevel = 0;
                    }
                    else
                    {
                        jitterBufferLevel -= consumedSamples;
                    }

                    if (i > 250 &&
                        jitterBufferLevel > 960 + 96 + (welf.getMean() + sqrt(welf.getVariance()) * 2) * 0.048)
                    {
                        logger::debug("eliminating", "");
                        auto newSize = eliminateSamples(audioBuf, 960, 40);
                        // autoCorrelate(audioBuf, 960);
                        // auto newSize = eliminateSamplesFade(audioBuf, 960, 24);
                        newSize = eliminateSilence(audioBuf, newSize);

                        jitterBufferLevel += newSize;
                        ::fwrite(audioBuf, 1, newSize * 2, audioPlayback.get());
                    }
                    else
                    {
                        jitterBufferLevel += 960;
                        ::fwrite(audioBuf, 1, 960 * 2, audioPlayback.get());
                    }
                }

                tracker.update(item.receiveTimestamp, rtpTimestamp);
                auto delay = jitt.update(item.receiveTimestamp, rtpTimestamp);
                welf.add(delay / utils::Time::us);

                csv.writeLine("%.3f, %.2f, %.2f",
                    ((item.receiveTimestamp - start) / utils::Time::ms) / 1000.0,
                    (jitterBufferLevel - 960) / 48.0,
                    (delay / utils::Time::us) / 1000.0);

                logger::debug("%.1f, jlvl %d, %.3fms, delay %.3f, wfstd %.3f, d%.3f",
                    "",
                    ((item.receiveTimestamp - start) / utils::Time::ms) / 1000.0,
                    jitterBufferLevel - 960,
                    (jitterBufferLevel - 960) / 48.0,
                    welf.getMean() / 1000.0,
                    sqrt(welf.getVariance()) / 1000.0,
                    (delay / utils::Time::us) / 1000.0);
            }
        }
    }
}
