#pragma once

#include "bridge/engine/SimulcastLevel.h"
#include "bridge/engine/SimulcastStream.h"
#include "concurrency/MpmcHashmap.h"
#include "concurrency/MpmcQueue.h"
#include "memory/List.h"
#include "memory/PriorityQueue.h"
#include "utils/StringBuilder.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bridge
{

struct EngineVideoStream;
struct EngineAudioStream;

class ActiveMediaList
{
public:
    static const size_t maxParticipants = 1024;

    /** The max number of considered active speakers for switching in to the active audio list per process iteration. */
    static const size_t numConsideredActiveSpeakers = 3;

    struct VideoScreenShareSsrcMapping
    {
        uint32_t _ssrc;
        uint32_t _rewriteSsrc;
    };

    ActiveMediaList(const std::vector<uint32_t>& audioSsrcs,
        const std::vector<SimulcastLevel>& videoSsrcs,
        const uint32_t defaultLastN);

    bool addAudioParticipant(const size_t endpointIdHash);
    bool removeAudioParticipant(const size_t endpointIdHash);
    bool addVideoParticipant(const size_t endpointIdHash,
        const SimulcastStream& simulcastStream,
        const utils::Optional<SimulcastStream>& secondarySimulcastStream);
    bool removeVideoParticipant(const size_t endpointIdHash);

    /**
     * @param level dBov levels adjusted to [0 .... 127] scale with 127 representing the highest volume (0 dBov) and
     * 0 representing lowest volume (-127 dBov)
     */
    inline void onNewAudioLevel(const size_t endpointIdHash, const uint8_t level)
    {
        if (level < 128)
        {
            _incomingAudioLevels.push({endpointIdHash, (uint8_t)(127 - level)});
        }
    }

    void process(const uint64_t timestampMs, bool& outDominantSpeakerChanged, bool& outUserMediaMapChanged);

    inline size_t getDominantSpeaker() const { return _dominantSpeaker; }

    inline const concurrency::MpmcHashmap32<size_t, uint32_t>& getAudioSsrcRewriteMap() const
    {
        return _audioSsrcRewriteMap;
    }

    inline const concurrency::MpmcHashmap32<size_t, SimulcastLevel>& getVideoSsrcRewriteMap() const
    {
        return _videoSsrcRewriteMap;
    }

    inline const concurrency::MpmcHashmap32<uint32_t, size_t>& getReverseVideoSsrcRewriteMap() const
    {
        return _reverseVideoSsrcRewriteMap;
    }

    inline const utils::Optional<std::pair<size_t, VideoScreenShareSsrcMapping>>& getVideoScreenShareSsrcMapping() const
    {
        return _videoScreenShareSsrcMapping;
    }

    inline bool isInActiveVideoList(const size_t endpointIdHash) const
    {
        return _activeVideoListLookupMap.contains(endpointIdHash);
    }

    inline bool isInUserActiveVideoList(const size_t endpointIdHash) const
    {
        const auto activeVideoListLookupMapItr = _activeVideoListLookupMap.find(endpointIdHash);

        if (_activeVideoListLookupMap.size() > _defaultLastN)
        {
            return activeVideoListLookupMapItr != _activeVideoListLookupMap.end() &&
                activeVideoListLookupMapItr->second && activeVideoListLookupMapItr->second->_previous;
        }
        else
        {
            return activeVideoListLookupMapItr != _activeVideoListLookupMap.end();
        }
    }

    inline bool getFeedbackSsrc(const uint32_t mainSsrc, uint32_t& outFeedbackSsrc) const
    {
        const auto videoFeedbackSsrcLookupMapItr = _videoFeedbackSsrcLookupMap.find(mainSsrc);
        if (videoFeedbackSsrcLookupMapItr == _videoFeedbackSsrcLookupMap.cend())
        {
            return false;
        }

        outFeedbackSsrc = videoFeedbackSsrcLookupMapItr->second;
        return true;
    }

    bool makeLastNListMessage(const size_t lastN,
        const size_t endpointIdHash,
        const size_t pinTargetEndpointIdHash,
        const concurrency::MpmcHashmap32<size_t, EngineVideoStream*>& engineVideoStreams,
        utils::StringBuilder<1024>& outMessage);

    bool makeUserMediaMapMessage(const size_t lastN,
        const size_t endpointIdHash,
        const size_t pinTargetEndpointIdHash,
        const concurrency::MpmcHashmap32<size_t, EngineAudioStream*>& engineAudioStreams,
        const concurrency::MpmcHashmap32<size_t, EngineVideoStream*>& engineVideoStreams,
        utils::StringBuilder<1024>& outMessage);

#if DEBUG
    void checkInvariant();
#endif

private:
    static const size_t intervalMs = 10;
    static const int32_t requiredConsecutiveWins = 3;
    // Only allow a new switch after 2s
    static const uint32_t maxSwitchDominantSpeakerEveryMs = 2000;
    // 100 entries corresponding to 2s for the case of 20ms packets
    static const size_t numLevels = 100;
    // Short Window (5 packets, typically 100ms) used to estimate noise level
    static const size_t lengthShortWindow = 5;

    struct AudioParticipant
    {
        AudioParticipant();

        static constexpr float decayOfMaxLevel = 0.006f;
        // Ramp up last seen noise level by 1 every second if no new minimum
        static constexpr float noiseLevelRampup = 0.01f;
        // Min should not be below -120 dBov
        static const uint8_t minNoiseLevel = 7;

        std::array<uint8_t, numLevels> _levels;
        size_t _index;
        size_t _indexEndShortWindow;

        int32_t _totalLevelLongWindow;
        int32_t _totalLevelShortWindow;
        int32_t _nonZeroLevelsLongWindow;
        int32_t _nonZeroLevelsShortWindow;
        float _maxRecentLevel;
        float _noiseLevel;
    };

    struct AudioLevelEntry
    {
        size_t _participant;
        uint8_t _level;
    };

    struct VideoParticipant
    {
        SimulcastStream _simulcastStream;
        utils::Optional<SimulcastStream> _secondarySimulcastStream;
    };

    struct AudioParticipantScore
    {
        size_t _participant;
        float _score;

        bool operator<(const AudioParticipantScore& rhs) const { return _score < rhs._score; }
        bool operator>(const AudioParticipantScore& rhs) const { return _score > rhs._score; }
        bool operator<=(const AudioParticipantScore& rhs) const { return _score <= rhs._score; }
        bool operator>=(const AudioParticipantScore& rhs) const { return _score >= rhs._score; }
    };

    uint32_t _defaultLastN;
    size_t _maxActiveListSize;

    concurrency::MpmcHashmap32<size_t, AudioParticipant> _audioParticipants;
    concurrency::MpmcQueue<AudioLevelEntry> _incomingAudioLevels;
    concurrency::MpmcQueue<uint32_t> _audioSsrcs;
    concurrency::MpmcHashmap32<size_t, uint32_t> _audioSsrcRewriteMap;
    memory::List<size_t, 32> _activeAudioList;

    size_t _dominantSpeaker;
    size_t _prevWinningDominantSpeaker;
    memory::PriorityQueue<AudioParticipantScore, maxParticipants> _highestScoringSpeakers;
    int32_t _consecutiveDominantSpeakerWins;

    concurrency::MpmcHashmap32<size_t, VideoParticipant> _videoParticipants;
    concurrency::MpmcQueue<SimulcastLevel> _videoSsrcs;
    concurrency::MpmcHashmap32<uint32_t, uint32_t> _videoFeedbackSsrcLookupMap;
    SimulcastLevel _videoScreenShareSsrc;
    concurrency::MpmcHashmap32<size_t, SimulcastLevel> _videoSsrcRewriteMap;
    concurrency::MpmcHashmap32<uint32_t, size_t> _reverseVideoSsrcRewriteMap;
    utils::Optional<std::pair<size_t, VideoScreenShareSsrcMapping>> _videoScreenShareSsrcMapping;
    memory::List<size_t, 32> _activeVideoList;
    concurrency::MpmcHashmap32<size_t, memory::List<size_t, 32>::Entry*> _activeVideoListLookupMap;

#if DEBUG
    std::atomic_uint32_t _reentrancyCounter;
#endif

    uint64_t _lastRunTimestampMs;
    uint64_t _lastChangeTimestampMs;

    bool updateActiveAudioList(const std::array<size_t, numConsideredActiveSpeakers>& highestScoringSpeakers);
    bool updateActiveVideoList(const size_t endpointIdHash);
};

} // namespace bridge
