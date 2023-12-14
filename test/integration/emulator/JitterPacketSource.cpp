#include "JitterPacketSource.h"
#include "logger/PacketLogger.h"
#include "rtp/RtpHeader.h"
#include "utils/Time.h"
#include <map>

namespace emulator
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
    while (reader.getNext(item))
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

JitterPacketSource::JitterPacketSource(memory::PacketPoolAllocator& allocator,
    uint32_t ptime,
    uint32_t jitterMs,
    uint32_t maxJitterMs)
    : _ptime(ptime),
      _audioSource(allocator, 50, emulator::Audio::Opus, ptime),
      _releaseTime(0),
      _audioTimeline(0),
      _packetLossRatio(0),
      _jitterMs(jitterMs),
      _maxJitterMs(maxJitterMs),
      _poissonDistribution(jitterMs),
      _randTwister(std::random_device{}())
{
}

void JitterPacketSource::openWithTone(double frequency, double onRatio)
{
    _audioSource.setFrequency(frequency);
    _audioSource.setVolume(0.2);
    _audioSource.enableIntermittentTone(onRatio);
}

memory::UniquePacket JitterPacketSource::getNext(uint64_t timestamp)
{
    if (!_nextPacket)
    {
        _nextPacket = _audioSource.getPacket(timestamp);
        if (!_nextPacket)
        {
            return nullptr;
        }

        _releaseTime = timestamp;
        _audioTimeline = timestamp;
    }

    if (utils::Time::diffGE(_releaseTime, timestamp, 0))
    {
        auto p = std::move(_nextPacket);
        _audioTimeline += _ptime * utils::Time::ms;
        _nextPacket = _audioSource.getPacket(_audioTimeline);
        assert(_nextPacket);

        const auto delayms = _poissonDistribution(_randTwister);

        _releaseTime = _audioTimeline + (_jitterMs > 0 ? std::min(delayms, _maxJitterMs) * utils::Time::ms : 0);

        return p;
    }

    return nullptr;
}

JitterTracePacketSource::JitterTracePacketSource(memory::PacketPoolAllocator& allocator)
    : _audioSource(allocator, 50, emulator::Audio::Opus, 20),
      _releaseTime(0),
      _sequenceOffset(0),
      _traceOffset(0),
      _audioTimeline(0),
      _eof(false),
      _packetLossRatio(0)
{
}

void JitterTracePacketSource::open(const char* audioPcm16File, const char* networkTrace)
{
    _traceReader = std::make_unique<logger::PacketLogReader>(::fopen(networkTrace, "r"));
    _audioSsrc = identifyAudioSsrc(*_traceReader);
    _traceReader->rewind();

    _audioSource.openPcm16File(audioPcm16File);
}

void JitterTracePacketSource::openWithTone(double frequency, const char* networkTrace)
{
    _traceReader = std::make_unique<logger::PacketLogReader>(::fopen(networkTrace, "r"));
    _audioSsrc = identifyAudioSsrc(*_traceReader);
    _traceReader->rewind();

    _audioSource.setFrequency(frequency);
    _audioSource.setVolume(0.6);
}

bool JitterTracePacketSource::getNextAudioTraceItem(logger::PacketLogItem& item)
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

memory::UniquePacket JitterTracePacketSource::getNext(uint64_t timestamp)
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

JitterPatternSource::JitterPatternSource(memory::PacketPoolAllocator& allocator,
    uint32_t ptime,
    const std::vector<uint32_t>& jitterPattern)
    : _ptime(ptime),
      _audioSource(allocator, 50, emulator::Audio::Opus, ptime),
      _releaseTime(0),
      _audioTimeline(0),
      _index(0),
      _jitterPattern(jitterPattern)
{
}

void JitterPatternSource::openWithTone(double frequency, double onRatio)
{
    _audioSource.setFrequency(frequency);
    _audioSource.setVolume(0.2);
    _audioSource.enableIntermittentTone(onRatio);
}

memory::UniquePacket JitterPatternSource::getNext(uint64_t timestamp)
{
    if (!_nextPacket)
    {
        _nextPacket = _audioSource.getPacket(timestamp);
        if (!_nextPacket)
        {
            return nullptr;
        }

        _releaseTime = timestamp;
        _audioTimeline = timestamp;
    }

    if (utils::Time::diffGE(_releaseTime, timestamp, 0))
    {
        auto p = std::move(_nextPacket);
        _audioTimeline += _ptime * utils::Time::ms;
        _nextPacket = _audioSource.getPacket(_audioTimeline);
        assert(_nextPacket);

        const auto delayms = _jitterPattern[_index++ % _jitterPattern.size()];

        _releaseTime = _audioTimeline + delayms * utils::Time::ms;

        return p;
    }

    return nullptr;
}

} // namespace emulator
