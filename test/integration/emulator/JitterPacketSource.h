#pragma once
#include "test/integration/emulator/AudioSource.h"
#include <random>

namespace logger
{
struct PacketLogItem;
class PacketLogReader;
} // namespace logger

namespace emulator
{
uint32_t identifyAudioSsrc(logger::PacketLogReader& reader);

class JitterPacketSource
{
public:
    JitterPacketSource(memory::PacketPoolAllocator& allocator,
        uint32_t ptime,
        uint32_t jitterMs = 45,
        uint32_t maxJitterMs = 80);

    void openWithTone(double frequency, double onRatio = 1.0);
    memory::UniquePacket getNext(uint64_t timestamp);

    void setRandomPacketLoss(double ratio) { _packetLossRatio = ratio; }

private:
    uint32_t _ptime;
    emulator::AudioSource _audioSource;
    memory::UniquePacket _nextPacket;
    uint64_t _releaseTime;
    uint64_t _audioTimeline;
    double _packetLossRatio;
    uint32_t _jitterMs;
    const uint32_t _maxJitterMs;
    std::poisson_distribution<uint32_t> _poissonDistribution;
    std::mt19937 _randTwister;
};

class JitterTracePacketSource
{
public:
    JitterTracePacketSource(memory::PacketPoolAllocator& allocator);

    void open(const char* audioPcm16File, const char* networkTrace);

    void openWithTone(double frequency, const char* networkTrace);

    bool getNextAudioTraceItem(logger::PacketLogItem& item);

    memory::UniquePacket getNext(uint64_t timestamp);

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

class JitterPatternSource
{
public:
    JitterPatternSource(memory::PacketPoolAllocator& allocator,
        uint32_t ptime,
        const std::vector<uint32_t>& jitterPattern);

    void openWithTone(double frequency, double onRatio = 1.0);
    memory::UniquePacket getNext(uint64_t timestamp);

private:
    uint32_t _ptime;
    emulator::AudioSource _audioSource;
    memory::UniquePacket _nextPacket;
    uint64_t _releaseTime;
    uint64_t _audioTimeline;
    uint32_t _index;
    std::vector<uint32_t> _jitterPattern;
};

} // namespace emulator
