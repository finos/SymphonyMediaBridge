
#include "codec/AudioReceivePipeline.h"
#include "codec/AudioTools.h"
#include "logger/Logger.h"
#include "logger/PacketLogger.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/JitterEstimator.h"
#include "rtp/JitterTracker.h"
#include "rtp/RtpHeader.h"
#include "test/integration/SampleDataUtils.h"
#include "test/integration/emulator/AudioSource.h"
#include "test/integration/emulator/JitterPacketSource.h"
#include "test/integration/emulator/TimeTurner.h"
#include "utils/Pacer.h"
#include "utils/ScopedFileHandle.h"
#include <gtest/gtest.h>
#include <random>

using namespace math;

namespace
{

} // namespace

class TimeTicker : public utils::TimeSource
{
public:
    TimeTicker() : _startTime(std::chrono::system_clock::now()), _time(utils::Time::getAbsoluteTime()) {}

    uint64_t getAbsoluteTime() const override { return _time; };

    void nanoSleep(uint64_t nanoSeconds) override { _time += nanoSeconds; };

    std::chrono::system_clock::time_point wallClock() const override
    {
        return _startTime + std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds(_time));
    }

    void advance(uint64_t nanoSeconds) override { _time += nanoSeconds; }

private:
    const std::chrono::system_clock::time_point _startTime;
    uint64_t _time;
};

class AudioPipelineTest : public testing::TestWithParam<std::string>
{
public:
    void SetUp() override { utils::Time::initialize(_timeTurner); }
    void TearDown() override { utils::Time::initialize(); }
    TimeTicker _timeTurner;
};

TEST_P(AudioPipelineTest, DISABLED_fileReRun)
{
    const uint32_t rtpFrequency = 48000;
    std::string trace = GetParam();
    memory::PacketPoolAllocator allocator(4096 * 4, "JitterTest");
    uint16_t* tmpZeroes = new uint16_t[960 * 50];
    std::fill(tmpZeroes, tmpZeroes + 960 * 10, 0);

    logger::info("scanning file %s", "", trace.c_str());

    emulator::JitterTracePacketSource psource(allocator);
    // psource.setRandomPacketLoss(0.03);
    psource.open("./_bwelogs/2minrecording.raw", ("./_bwelogs/" + trace).c_str());

    utils::ScopedFileHandle audioPlayback(::fopen(("/mnt/c/dev/rtc/" + trace + "out.raw").c_str(), "w+"));

    auto pipeline = std::make_unique<codec::AudioReceivePipeline>(48000, 20, 100, 1);

    const auto samplesPerPacket = rtpFrequency / 50;
    uint32_t extendedSequenceNumber = 0;

    utils::Pacer playbackPacer(utils::Time::ms * 20);
    playbackPacer.reset(100);

    for (uint64_t timeSteps = 0; timeSteps < 90000 && !psource.isEof(); ++timeSteps)
    {
        _timeTurner.advance(utils::Time::ms);
        const auto timestamp = utils::Time::getAbsoluteTime();
        for (auto packet = psource.getNext(timestamp); packet; packet = psource.getNext(timestamp))
        {
            auto header = rtp::RtpHeader::fromPacket(*packet);
            uint16_t curExtSeq = extendedSequenceNumber & 0xFFFFu;
            int16_t adv = header->sequenceNumber.get() - curExtSeq;

            const auto acceptedPacket =
                pipeline->onRtpPacket(extendedSequenceNumber + adv, std::move(packet), timestamp);
            if (adv > 0)
            {
                extendedSequenceNumber += adv;
            }
            if (!acceptedPacket)
            {
                logger::warn("JB full packet dropped %u", "", extendedSequenceNumber);
            }
        }

        if (pipeline->needProcess())
        {
            pipeline->process(timestamp);
        }

        if (playbackPacer.timeToNextTick(timestamp) <= 0)
        {
            pipeline->fetchStereo(samplesPerPacket);

            ::fwrite(pipeline->getAudio(), samplesPerPacket * 2, sizeof(int16_t), audioPlayback.get());

            playbackPacer.tick(timestamp);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(AudioPipelineRerun,
    AudioPipelineTest,
    ::testing::Values("Transport-6-4G-1-5Mbps",
        "Transport-22-4G-2.3Mbps",
        "Transport-30-3G-1Mbps",
        "Transport-32_Idre",
        "Transport-44-clkdrift",
        "Transport-48_80_3G",
        "Transport-3887-wifi",
        "Transport-105_tcp_1ploss",
        "Transport-1094-4G",
        "Transport-14-wifi",
        "Transport-48_50_3G",
        "Transport-86_tcp_1ploss"));

TEST_F(AudioPipelineTest, DTX)
{
    const uint32_t rtpFrequency = 48000;
    memory::PacketPoolAllocator allocator(4096 * 4, "JitterTest");

    auto pipeline = std::make_unique<codec::AudioReceivePipeline>(48000, 20, 100, 1);

    const auto samplesPerPacket = rtpFrequency / 50;
    uint32_t extendedSequenceNumber = 0;
    uint32_t timestampCounter = 4000;

    utils::Pacer playbackPacer(utils::Time::ms * 20);
    playbackPacer.reset(100);

    emulator::JitterPacketSource audioSource(allocator, 20);
    audioSource.openWithTone(210, 0.34);
    uint32_t underruns = 0;

    for (uint64_t timeSteps = 0; timeSteps < 9000; ++timeSteps)
    {
        _timeTurner.advance(utils::Time::ms * 2);
        const auto timestamp = utils::Time::getAbsoluteTime();
        if (pipeline->needProcess())
        {
            pipeline->process(timestamp);
        }

        if (playbackPacer.timeToNextTick(timestamp) <= 0)
        {
            if (pipeline->needProcess())
            {
                ++underruns;
            }
            auto fetched = pipeline->fetchStereo(samplesPerPacket);
            playbackPacer.tick(timestamp);
            if (fetched == 0)
            {
                logger::info("no audio", "test");
            }
        }

        auto packet = audioSource.getNext(timestamp);
        if (!packet)
        {
            continue;
        }
        if (timestampCounter == 4000 + 960 * 20)
        {
            timestampCounter += 960;
            logger::info("dtx at %u", "test", timestampCounter);
            continue;
        }
        auto header = rtp::RtpHeader::fromPacket(*packet);
        header->sequenceNumber = extendedSequenceNumber++ & 0xFFFFu;
        header->timestamp = timestampCounter;
        timestampCounter += samplesPerPacket;

        pipeline->onRtpPacket(extendedSequenceNumber, std::move(packet), timestamp);
    }

    EXPECT_LT(underruns, 50);
}

TEST_F(AudioPipelineTest, ptime10)
{
    const uint32_t rtpFrequency = 48000;
    memory::PacketPoolAllocator allocator(4096 * 4, "JitterTest");

    auto pipeline = std::make_unique<codec::AudioReceivePipeline>(48000, 20, 100, 1);

    uint32_t extendedSequenceNumber = 0;
    uint32_t timestampCounter = 4000;

    utils::Pacer playbackPacer(utils::Time::ms * 20);
    playbackPacer.reset(100);

    emulator::JitterPacketSource audioSource(allocator, 10);
    audioSource.openWithTone(210, 0.34);

    uint32_t underruns = 0;

    const uint32_t samplesPerPacketSent = 10 * rtpFrequency / 1000;
    for (uint64_t timeSteps = 0; timeSteps < 9000; ++timeSteps)
    {
        _timeTurner.advance(utils::Time::ms * 2);
        const auto timestamp = utils::Time::getAbsoluteTime();
        if (pipeline->needProcess())
        {
            pipeline->process(timestamp);
        }

        const auto samplesPerPacketFetch = 20 * rtpFrequency / 1000;
        if (playbackPacer.timeToNextTick(timestamp) <= 0)
        {
            if (pipeline->needProcess())
            {
                ++underruns;
            }
            pipeline->fetchStereo(samplesPerPacketFetch);
            playbackPacer.tick(timestamp);
        }

        auto packet = audioSource.getNext(timestamp);
        if (!packet)
        {
            continue;
        }
        if (timestampCounter == 4000 + 960 * 20)
        {
            timestampCounter += samplesPerPacketSent;
            logger::info("dtx at %u", "test", timestampCounter);
            continue;
        }
        auto header = rtp::RtpHeader::fromPacket(*packet);
        header->sequenceNumber = extendedSequenceNumber++ & 0xFFFFu;
        header->timestamp = timestampCounter;
        timestampCounter += samplesPerPacketSent;
        ;

        pipeline->onRtpPacket(extendedSequenceNumber, std::move(packet), timestamp);
    }

    EXPECT_LT(underruns, 70);
}

TEST_F(AudioPipelineTest, ContinuousTone)
{
    const uint32_t rtpFrequency = 48000;
    memory::PacketPoolAllocator allocator(4096 * 4, "JitterTest");

    auto pipeline = std::make_unique<codec::AudioReceivePipeline>(48000, 20, 100, 1);

    const auto samplesPerPacket = rtpFrequency / 50;
    uint32_t extendedSequenceNumber = 0;

    utils::Pacer playbackPacer(utils::Time::ms * 20);
    playbackPacer.reset(100);

    emulator::JitterPacketSource audioSource(allocator, 20, 43);
    audioSource.openWithTone(800, 1.0);
    uint32_t underruns = 0;

    utils::ScopedFileHandle dumpFile(nullptr); //(::fopen("/mnt/c/dev/rtc/ContinuousTone.raw", "wr"));

    for (uint64_t timeSteps = 0; timeSteps < 9000; ++timeSteps)
    {
        _timeTurner.advance(utils::Time::ms * 2);
        const auto timestamp = utils::Time::getAbsoluteTime();
        if (pipeline->needProcess())
        {
            pipeline->process(timestamp);
        }

        if (playbackPacer.timeToNextTick(timestamp) <= 0)
        {
            if (pipeline->needProcess())
            {
                ++underruns;
            }
            auto fetched = pipeline->fetchStereo(samplesPerPacket);
            playbackPacer.tick(timestamp);
            if (fetched == 0)
            {
                logger::info("no audio", "test");
                int16_t silence[samplesPerPacket * 2];
                std::memset(silence, 0, samplesPerPacket * 2 * sizeof(int16_t));
                if (dumpFile)
                {
                    SampleDataUtils::dumpPayload(dumpFile.get(), silence, samplesPerPacket * 2);
                }
            }
            else
            {
                if (dumpFile)
                {
                    SampleDataUtils::dumpPayload(dumpFile.get(), pipeline->getAudio(), fetched * 2);
                }
            }
        }

        auto packet = audioSource.getNext(timestamp);
        if (!packet)
        {
            continue;
        }
        auto header = rtp::RtpHeader::fromPacket(*packet);

        int16_t advance = static_cast<int16_t>(header->timestamp.get() - (extendedSequenceNumber & 0xFFFFu));
        extendedSequenceNumber += advance;

        pipeline->onRtpPacket(extendedSequenceNumber, std::move(packet), timestamp);
    }

    EXPECT_LE(underruns, 30);
}

TEST_F(AudioPipelineTest, lowJitter)
{
    const uint32_t rtpFrequency = 48000;
    memory::PacketPoolAllocator allocator(4096 * 4, "JitterTest");

    auto pipeline = std::make_unique<codec::AudioReceivePipeline>(48000, 20, 100, 1);

    uint32_t extendedSequenceNumber = 0;

    utils::Pacer playbackPacer(utils::Time::ms * 20);
    playbackPacer.reset(_timeTurner.getAbsoluteTime() + utils::Time::ms * 4); // consume 10ms after capture

    emulator::JitterPacketSource audioSource(allocator, 20, 14, 41);
    audioSource.openWithTone(210, 0.34);
    {
        auto firstPacket = audioSource.getNext(_timeTurner.getAbsoluteTime()); // reset audio time line
        auto header = rtp::RtpHeader::fromPacket(*firstPacket);
        extendedSequenceNumber = header->sequenceNumber;
    }
    uint32_t underruns = 0;

    for (uint64_t timeSteps = 0; timeSteps < 9000; ++timeSteps)
    {
        _timeTurner.advance(utils::Time::ms * 1);
        const auto timestamp = utils::Time::getAbsoluteTime();
        if (pipeline->needProcess())
        {
            pipeline->process(timestamp);
        }

        const auto samplesPerPacketFetch = 20 * rtpFrequency / 1000;
        if (playbackPacer.timeToNextTick(timestamp) <= 0)
        {
            if (pipeline->needProcess())
            {
                ++underruns;
            }
            pipeline->fetchStereo(samplesPerPacketFetch);
            playbackPacer.tick(timestamp);
        }

        while (auto packet = audioSource.getNext(timestamp))
        {
            auto header = rtp::RtpHeader::fromPacket(*packet);
            auto adv = static_cast<int16_t>(
                header->sequenceNumber.get() - static_cast<uint16_t>(extendedSequenceNumber & 0xFFFFu));
            extendedSequenceNumber += adv;
            // logger::debug("pkt produced %zu", "", timestamp / utils::Time::ms);
            pipeline->onRtpPacket(extendedSequenceNumber, std::move(packet), timestamp);
        }
    }

    EXPECT_LT(underruns, 7);
}

TEST_F(AudioPipelineTest, nearEmpty)
{
    const uint32_t rtpFrequency = 48000;
    memory::PacketPoolAllocator allocator(4096 * 4, "JitterTest");

    auto pipeline = std::make_unique<codec::AudioReceivePipeline>(48000, 20, 100, 1);

    uint32_t extendedSequenceNumber = 0;

    utils::Pacer playbackPacer(utils::Time::ms * 20);
    playbackPacer.reset(_timeTurner.getAbsoluteTime() + utils::Time::ms * 3); // consume 10ms after capture

    // clang-format off
    std::vector<uint32_t> jitter =
        {0, 0, 0, 0, 0, 1, 0, 0, 0, 19, 1, 0, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,
        0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,
        0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,
        0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,
        0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1,0, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 1, 0, 1, 1};
    // clang-format on

    emulator::JitterPatternSource audioSource(allocator, 20, jitter);
    audioSource.openWithTone(210, 0.34);
    {
        auto firstPacket = audioSource.getNext(_timeTurner.getAbsoluteTime()); // reset audio time line
        auto header = rtp::RtpHeader::fromPacket(*firstPacket);
        extendedSequenceNumber = header->sequenceNumber;
    }
    uint32_t underruns = 0;
    const auto samplesPerPacketFetch = 20 * rtpFrequency / 1000;

    for (uint64_t timeSteps = 0; timeSteps < jitter.size() * 20; ++timeSteps)
    {
        _timeTurner.advance(utils::Time::ms);
        const auto timestamp = utils::Time::getAbsoluteTime();
        if (pipeline->needProcess())
        {
            pipeline->process(timestamp);
        }

        if (playbackPacer.timeToNextTick(timestamp) <= 0)
        {
            if (pipeline->needProcess())
            {
                ++underruns;
            }
            pipeline->fetchStereo(samplesPerPacketFetch);
            playbackPacer.tick(timestamp);
        }

        while (auto packet = audioSource.getNext(timestamp))
        {
            auto header = rtp::RtpHeader::fromPacket(*packet);
            auto adv = static_cast<int16_t>(
                header->sequenceNumber.get() - static_cast<uint16_t>(extendedSequenceNumber & 0xFFFFu));
            extendedSequenceNumber += adv;
            pipeline->onRtpPacket(extendedSequenceNumber, std::move(packet), timestamp);
        }
    }

    EXPECT_EQ(underruns, 1);
    _timeTurner.advance(40 * utils::Time::ms);
    pipeline->process(utils::Time::getAbsoluteTime());
    EXPECT_EQ(pipeline->fetchStereo(samplesPerPacketFetch), 960);
    pipeline->process(utils::Time::getAbsoluteTime());
    EXPECT_EQ(pipeline->fetchStereo(samplesPerPacketFetch), 0);
}

TEST_F(AudioPipelineTest, everIncreasing)
{
    const uint32_t rtpFrequency = 48000;
    memory::PacketPoolAllocator allocator(4096 * 4, "JitterTest");

    auto pipeline = std::make_unique<codec::AudioReceivePipeline>(48000, 20, 100, 1);

    uint32_t extendedSequenceNumber = 0;

    utils::Pacer playbackPacer(utils::Time::ms * 20);
    playbackPacer.reset(_timeTurner.getAbsoluteTime() + utils::Time::ms * 3); // consume 10ms after capture

    std::vector<uint32_t> jitter = {0, 0, 0, 10, 20, 60, 90, 120};
    for (int i = 0; i < 310; ++i)
    {
        jitter.push_back(120 + i * 90);
    }

    emulator::JitterPatternSource audioSource(allocator, 20, jitter);
    audioSource.openWithTone(210, 0.34);
    {
        auto firstPacket = audioSource.getNext(_timeTurner.getAbsoluteTime()); // reset audio time line
        auto header = rtp::RtpHeader::fromPacket(*firstPacket);
        extendedSequenceNumber = header->sequenceNumber;
    }
    uint32_t underruns = 0;
    const auto samplesPerPacketFetch = 20 * rtpFrequency / 1000;

    for (uint64_t timeSteps = 0; timeSteps < 33000; ++timeSteps)
    {
        _timeTurner.advance(utils::Time::ms);
        const auto timestamp = utils::Time::getAbsoluteTime();
        if (pipeline->needProcess())
        {
            pipeline->process(timestamp);
        }

        if (playbackPacer.timeToNextTick(timestamp) <= 0)
        {
            if (pipeline->needProcess())
            {
                ++underruns;
            }
            pipeline->fetchStereo(samplesPerPacketFetch);
            playbackPacer.tick(timestamp);
        }

        while (auto packet = audioSource.getNext(timestamp))
        {
            auto header = rtp::RtpHeader::fromPacket(*packet);
            auto adv = static_cast<int16_t>(
                header->sequenceNumber.get() - static_cast<uint16_t>(extendedSequenceNumber & 0xFFFFu));
            extendedSequenceNumber += adv;
            bool posted = pipeline->onRtpPacket(extendedSequenceNumber, std::move(packet), timestamp);
            if (timeSteps == 32969)
            {
                ASSERT_FALSE(posted);
            }
            else
            {
                ASSERT_TRUE(posted);
            }
        }
    }

    EXPECT_LT(underruns, 1650);
    EXPECT_GE(underruns, 1600);
}
