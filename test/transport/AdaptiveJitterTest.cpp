
#include "bwe/BandwidthEstimator.h"
#include "codec/AudioFader.h"
#include "codec/AudioReceivePipeline.h"
#include "codec/AudioTools.h"
#include "codec/OpusDecoder.h"
#include "concurrency/MpmcQueue.h"
#include "logger/Logger.h"
#include "logger/PacketLogger.h"
#include "math/Matrix.h"
#include "math/WelfordVariance.h"
#include "memory/AudioPacketPoolAllocator.h"
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
#include "test/integration/emulator/AudioSource.h"
#include "test/integration/emulator/TimeTurner.h"
#include "test/transport/NetworkLink.h"
#include "utils/Pacer.h"
#include <gtest/gtest.h>

using namespace math;

#include "utils/ScopedFileHandle.h"
#include <gtest/gtest.h>
#include <random>

using namespace math;

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
} // namespace

class TimeTicker : public utils::TimeSource
{
public:
    TimeTicker() : _startTime(std::chrono::system_clock::now()), _time(utils::Time::getAbsoluteTime()) {}

    uint64_t getAbsoluteTime() override { return _time; };

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
    void ShutDown() { utils::Time::initialize(); }
    TimeTicker _timeTurner;
};

class JitterTracePacketSource
{
public:
    JitterTracePacketSource(memory::PacketPoolAllocator& allocator)
        : _audioSource(allocator, 50, emulator::Audio::Opus, 20),
          _releaseTime(0),
          _sequenceOffset(0),
          _traceOffset(0),
          _audioTimeline(0),
          _eof(false),
          _packetLossRatio(0)
    {
    }

    void open(const char* audioPcm16File, const char* networkTrace)
    {
        _traceReader = std::make_unique<logger::PacketLogReader>(::fopen(networkTrace, "r"));
        _audioSsrc = identifyAudioSsrc(*_traceReader);
        _traceReader->rewind();

        _audioSource.openPcm16File(audioPcm16File);
    }

    void openWithTone(double frequency, const char* networkTrace)
    {
        _traceReader = std::make_unique<logger::PacketLogReader>(::fopen(networkTrace, "r"));
        _audioSsrc = identifyAudioSsrc(*_traceReader);
        _traceReader->rewind();

        _audioSource.setFrequency(frequency);
        _audioSource.setVolume(0.6);
    }

    bool getNextAudioTraceItem(logger::PacketLogItem& item)
    {
        for (; _traceReader->getNext(item);)
        {
            if (item.ssrc == _audioSsrc && rand() % 1000 > _packetLossRatio * 1000.0)
            {
                return true;
            }
        }

        return false;
    }

    memory::UniquePacket getNext(uint64_t timestamp)
    {
        if (!_nextPacket)
        {
            logger::PacketLogItem item;
            if (!getNextAudioTraceItem(item))
            {
                _eof = true;
                return nullptr;
            }

            uint16_t audioSeqNo = _audioSource.getSequenceCounter();
            if (_traceOffset != 0)
            {
                int16_t diff = item.sequenceNumber + _sequenceOffset - audioSeqNo;
                if (diff != 0)
                {
                    logger::debug("ssrc %u, seq %u, audio seq %u",
                        "",
                        item.ssrc,
                        item.sequenceNumber + _sequenceOffset,
                        audioSeqNo);
                }
                if (diff < 0)
                {
                    return nullptr; // reorder
                }
            }

            auto packet = _audioSource.getPacket(_audioTimeline);
            auto header = rtp::RtpHeader::fromPacket(*packet);
            if (_traceOffset == 0)
            {
                _sequenceOffset = header->sequenceNumber - item.sequenceNumber;
                _traceOffset = timestamp - item.receiveTimestamp;
            }

            _audioTimeline += 20 * utils::Time::ms;

            while (header->sequenceNumber != item.sequenceNumber + _sequenceOffset)
            {
                logger::debug("loss in audio source seq %u, trace %u",
                    "JitterTracePacketSource",
                    header->sequenceNumber.get(),
                    item.sequenceNumber + _sequenceOffset);
                packet = _audioSource.getPacket(_audioTimeline);
                header = rtp::RtpHeader::fromPacket(*packet);
                _audioTimeline += 20 * utils::Time::ms;
            }

            _nextPacket = std::move(packet);
            _releaseTime = item.receiveTimestamp + _traceOffset;
        }

        if (utils::Time::diffGE(_releaseTime, timestamp, 0))
        {
            return std::move(_nextPacket);
        }
        return nullptr;
    }

    bool isEof() const { return _eof; }

    void setRandomPacketLoss(double ratio) { _packetLossRatio = ratio; }

private:
    emulator::AudioSource _audioSource;
    memory::UniquePacket _nextPacket;
    uint64_t _releaseTime;
    std::unique_ptr<logger::PacketLogReader> _traceReader;
    int16_t _sequenceOffset;

    int64_t _traceOffset;
    uint32_t _audioSsrc;
    uint64_t _audioTimeline;
    bool _eof;
    double _packetLossRatio;
};

class JitterPacketSource
{
public:
    JitterPacketSource(memory::PacketPoolAllocator& allocator, uint32_t ptime)
        : _ptime(ptime),
          _audioSource(allocator, 50, emulator::Audio::Opus, ptime),
          _releaseTime(0),
          _audioTimeline(0),
          _packetLossRatio(0)
    {
    }

    void openWithTone(double frequency, double onRatio = 1.0)
    {
        _audioSource.setFrequency(frequency);
        _audioSource.setVolume(0.2);
        _audioSource.enableIntermittentTone(onRatio);
    }

    memory::UniquePacket getNext(uint64_t timestamp)
    {
        if (!_nextPacket)
        {
            _nextPacket = _audioSource.getPacket(timestamp);
            if (!_nextPacket)
            {
                return nullptr;
            }
            _releaseTime = timestamp + (rand() % 45) * utils::Time::ms;
            _audioTimeline = timestamp;
        }

        if (static_cast<int64_t>(timestamp - _releaseTime) >= 0)
        {
            auto p = std::move(_nextPacket);
            _audioTimeline += _ptime * utils::Time::ms;
            _nextPacket = _audioSource.getPacket(_audioTimeline);
            assert(_nextPacket);

            _releaseTime = _audioTimeline + (rand() % 45) * utils::Time::ms;
            return p;
        }

        return nullptr;
    }

    void setRandomPacketLoss(double ratio) { _packetLossRatio = ratio; }

private:
    uint32_t _ptime;
    emulator::AudioSource _audioSource;
    memory::UniquePacket _nextPacket;
    uint64_t _releaseTime;
    std::unique_ptr<logger::PacketLogReader> _traceReader;
    uint64_t _audioTimeline;
    double _packetLossRatio;
};

TEST_P(AudioPipelineTest, DISABLED_fileReRun)
{
    const uint32_t rtpFrequency = 48000;
    std::string trace = GetParam();
    memory::PacketPoolAllocator allocator(4096 * 4, "JitterTest");
    uint16_t* tmpZeroes = new uint16_t[960 * 50];
    std::fill(tmpZeroes, tmpZeroes + 960 * 10, 0);

    logger::info("scanning file %s", "", trace.c_str());

    JitterTracePacketSource psource(allocator);
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
                pipeline->onRtpPacket(extendedSequenceNumber + adv, std::move(packet), timestamp, true);
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
            pipeline->process(timestamp, true);
        }

        if (playbackPacer.timeToNextTick(timestamp) <= 0)
        {
            pipeline->fetchStereo(samplesPerPacket);

            ::fwrite(pipeline->getAudio(), samplesPerPacket * 2, sizeof(int16_t), audioPlayback.get());

            playbackPacer.tick(timestamp);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(ArpFileReRun,
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
        "Transport-42-clkdrift",
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

    JitterPacketSource audioSource(allocator, 20);
    audioSource.openWithTone(210, 0.34);
    uint32_t underruns = 0;

    for (uint64_t timeSteps = 0; timeSteps < 9000; ++timeSteps)
    {
        _timeTurner.advance(utils::Time::ms * 2);
        const auto timestamp = utils::Time::getAbsoluteTime();
        if (pipeline->needProcess())
        {
            pipeline->process(timestamp, true);
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

        pipeline->onRtpPacket(extendedSequenceNumber, std::move(packet), timestamp, true);
    }

    EXPECT_LT(underruns, 7);
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

    JitterPacketSource audioSource(allocator, 10);
    audioSource.openWithTone(210, 0.34);

    uint32_t underruns = 0;

    const uint32_t samplesPerPacketSent = 10 * rtpFrequency / 1000;
    for (uint64_t timeSteps = 0; timeSteps < 9000; ++timeSteps)
    {
        _timeTurner.advance(utils::Time::ms * 2);
        const auto timestamp = utils::Time::getAbsoluteTime();
        if (pipeline->needProcess())
        {
            pipeline->process(timestamp, true);
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

        pipeline->onRtpPacket(extendedSequenceNumber, std::move(packet), timestamp, true);
    }

    EXPECT_LT(underruns, 5);

    // send through a few packets then gap in rtp timestamp but in sequence

    // make sure the audio data reflects this
}
