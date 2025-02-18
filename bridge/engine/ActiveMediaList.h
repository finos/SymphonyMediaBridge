#pragma once

#include "api/SimulcastGroup.h"
#include "bridge/engine/ActiveTalker.h"
#include "bridge/engine/BarbellEndpointMap.h"
#include "bridge/engine/NeighbourMembership.h"
#include "bridge/engine/SimulcastLevel.h"
#include "bridge/engine/SimulcastStream.h"
#include "concurrency/MpmcHashmap.h"
#include "concurrency/MpmcPublish.h"
#include "concurrency/MpmcQueue.h"
#include "memory/List.h"
#include "utils/Time.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace utils
{
template <size_t S>
class StringBuilder;
} // namespace utils

namespace bridge
{

struct EngineVideoStream;
struct EngineAudioStream;

class ActiveMediaList
{
public:
    static constexpr size_t maxParticipants = 2048;

    struct VideoScreenShareSsrcMapping
    {
        uint32_t ssrc;
        uint32_t rewriteSsrc;
    };

    ActiveMediaList(size_t instanceId,
        const std::vector<uint32_t>& audioSsrcs,
        const std::vector<api::SimulcastGroup>& videoSsrcs,
        const uint32_t defaultLastN,
        uint32_t audioLastN,
        uint32_t activeTalkerSilenceThresholdDb);

    bool addAudioParticipant(const size_t endpointIdHash, const char* endpointId);
    bool addBarbellAudioParticipant(const size_t endpointIdHash,
        const char* endpointId,
        float noiseLevel,
        float recentLevel);
    bool removeAudioParticipant(const size_t endpointIdHash);
    bool addVideoParticipant(const size_t endpointIdHash,
        const SimulcastStream& simulcastStream,
        const utils::Optional<SimulcastStream>& secondarySimulcastStream,
        const char* endpointId);
    bool addBarbellVideoParticipant(const size_t endpointIdHash,
        const SimulcastStream& simulcastStream,
        const utils::Optional<SimulcastStream>& secondarySimulcastStream,
        const char* endpointId);
    bool removeVideoParticipant(const size_t endpointIdHash);

    /**
     * @param level dBov levels adjusted to [0 .... 127] scale with 127 representing the highest volume (0 dBov) and
     * 0 representing lowest volume (-127 dBov)
     */
    inline void onNewAudioLevel(const size_t endpointIdHash, const uint8_t level, bool ptt)
    {
        if (level < 128)
        {
            _incomingAudioLevels.push({endpointIdHash, (uint8_t)(127 - level), ptt});
        }
    }

    void process(const uint64_t timestamp,
        bool& outDominantSpeakerChanged,
        bool& outUserMediaMapChanged,
        bool& outAudioMapChanged);

    inline size_t getDominantSpeaker() const { return _dominantSpeaker; }

    void makeDominantSpeakerMessage(utils::StringBuilder<256>& outMessage);

    const std::map<size_t, ActiveTalker> getActiveTalkers() const;

    inline const concurrency::MpmcHashmap32<size_t, uint32_t>& getAudioSsrcRewriteMap() const
    {
        return _audioSsrcRewriteMap;
    }

    inline const concurrency::MpmcHashmap32<size_t, api::SimulcastGroup>& getVideoSsrcRewriteMap() const
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

    inline bool isInActiveTalkerList(const size_t endpointIdHash) const
    {
        return _audioSsrcRewriteMap.contains(endpointIdHash);
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
        utils::StringBuilder<1024>& outMessage);

    bool makeUserMediaMapMessage(const size_t lastN,
        const size_t endpointIdHash,
        const size_t pinTargetEndpointIdHash,
        const concurrency::MpmcHashmap32<size_t, EngineVideoStream*>& engineVideoStreams,
        utils::StringBuilder<1024>& outMessage);

    bool makeBarbellUserMediaMapMessage(utils::StringBuilder<1024>& outMessage,
        const engine::EndpointMembershipsMap& membershipMap,
        bool includeVideo);

    uint32_t getMapRevision() const { return _ssrcMapRevision; }
#if DEBUG
    void checkInvariant();
#endif

    void logAudioList();

private:
    static const size_t INTERVAL_MS = 10;
    // Only allow a new switch after 2s
    static const uint64_t minSpotlightDuration = 2000 * utils::Time::ms;

    struct AudioParticipant
    {
        AudioParticipant(const char* id);
        AudioParticipant(const char* id, float noiseLevel, float recentLevel);

        static constexpr float MAX_LEVEL_DECAY = 0.006f;
        // Ramp up last seen noise level by 1 every second if no new minimum
        static constexpr float NOISE_RAMPUP = 0.01f;
        // Min should not be below -120 dBov
        static const float MIN_NOISE;

        void setNoiseLevel(float level)
        {
            noiseLevel = level;
            history.fill(level);
        }

        float getScore() const { return std::max(0.0f, maxRecentLevel - noiseLevel); }
        float getInstantScore() const { return std::max(0.0f, audioLevel - noiseLevel); }
        void onNewLevel(uint8_t level, uint64_t timestamp);

        class History
        {
        public:
            void update(uint8_t level, uint64_t timestamp);
            float average() const { return static_cast<float>(_totalLevel) / _levels.size(); }
            void fill(uint8_t level);
            bool allNonZero() const { return _nonZeroLevels == _levels.size(); }
            uint64_t getUpdateTime() const { return _timestamp; }

        private:
            std::array<uint8_t, 5> _levels = {0};
            size_t _index = 0;
            uint32_t _totalLevel = 0;
            uint32_t _nonZeroLevels = 0;
            uint64_t _timestamp = 0;

        } history;

        float audioLevel;
        float maxRecentLevel;
        float noiseLevel;
        bool ptt;
        EndpointIdString endpointId;
        const bool isLocal;
    };

    struct AudioLevelEntry
    {
        size_t participant;
        uint8_t level;
        bool ptt;
    };

    struct VideoParticipant
    {
        VideoParticipant(const char* id,
            const SimulcastStream& primaryStream,
            const utils::Optional<SimulcastStream>& secondaryStream,
            bool isLocal)
            : isLocal(isLocal),
              endpointId(id),
              simulcastStream(primaryStream),
              secondarySimulcastStream(secondaryStream)
        {
        }

        bool isLocal;
        EndpointIdString endpointId;
        SimulcastStream simulcastStream;
        utils::Optional<SimulcastStream> secondarySimulcastStream;
    };

    struct AudioParticipantScore
    {
        size_t participant;
        float score;
        float noiseLevel;

        bool operator<(const AudioParticipantScore& rhs) const { return score < rhs.score; }
        bool operator>(const AudioParticipantScore& rhs) const { return score > rhs.score; }
        bool operator<=(const AudioParticipantScore& rhs) const { return score <= rhs.score; }
        bool operator>=(const AudioParticipantScore& rhs) const { return score >= rhs.score; }
    };

    logger::LoggableId _logId;
    const uint32_t _defaultLastN;
    const size_t _maxActiveListSize;
    const size_t _audioLastN;
    const uint32_t _activeTalkerSilenceThresholdDb;
    const size_t _maxSpeakers;

    concurrency::MpmcHashmap32<size_t, AudioParticipant> _audioParticipants;

    template <size_t MAX_SIZE>
    struct ActiveTalkersSnapshot
    {
        std::array<ActiveTalker, MAX_SIZE> activeTalker;
        static const size_t maxSize = MAX_SIZE;
        size_t count = 0;
    };
    using TActiveTalkersSnapshot = ActiveTalkersSnapshot<maxParticipants / 2>;

    // Use 6 to accommodate 1 writing thread for "process" and up to 5 http threads.
    concurrency::MpmcPublish<TActiveTalkersSnapshot, 6> _activeTalkerSnapshot;

    concurrency::MpmcQueue<AudioLevelEntry> _incomingAudioLevels;
    concurrency::MpmcQueue<uint32_t> _audioSsrcs;
    concurrency::MpmcHashmap32<size_t, uint32_t> _audioSsrcRewriteMap;
    memory::List<size_t, 32> _activeAudioList;

    std::atomic_size_t _dominantSpeaker;
    size_t _nominatedSpeaker;
    std::array<AudioParticipantScore, maxParticipants> _highestScoringSpeakers;

    concurrency::MpmcHashmap32<size_t, VideoParticipant> _videoParticipants;
    concurrency::MpmcQueue<api::SimulcastGroup> _videoSsrcs;
    concurrency::MpmcHashmap32<uint32_t, uint32_t> _videoFeedbackSsrcLookupMap;
    api::SsrcPair _videoScreenShareSsrc;
    concurrency::MpmcHashmap32<size_t, api::SimulcastGroup> _videoSsrcRewriteMap;
    concurrency::MpmcHashmap32<uint32_t, size_t> _reverseVideoSsrcRewriteMap;
    utils::Optional<std::pair<size_t, VideoScreenShareSsrcMapping>> _videoScreenShareSsrcMapping;
    memory::List<size_t, 32> _activeVideoList;
    concurrency::MpmcHashmap32<size_t, memory::List<size_t, 32>::Entry*> _activeVideoListLookupMap;

#if DEBUG
    std::atomic_uint32_t _reentrancyCounter;
#endif

    uint64_t _lastRunTimestamp;
    uint64_t _dominationTimestamp;
    uint64_t _nominationTimestamp;
    uint32_t _ssrcMapRevision;
    uint32_t _transactionCounter;

    size_t rankSpeakers();
    void updateLevels(const uint64_t timestampMs);
    bool updateActiveAudioList(size_t endpointIdHash);
    bool updateActiveVideoList(const size_t endpointIdHash);
    void addToVideoRewriteMap(size_t endpointIdHash, api::SimulcastGroup simulcastGroup);
    void removeFromRewriteMap(size_t endpointIdHash);

    bool onAudioParticipantAdded(const size_t endpointIdHash, const char* endpointId);
    bool onVideoParticipantAdded(const size_t endpointIdHash,
        const SimulcastStream& simulcastStream,
        const utils::Optional<SimulcastStream>& secondarySimulcastStream,
        const char* endpointId);
};

} // namespace bridge
