#include "bridge/engine/ActiveMediaList.h"
#include "api/DataChannelMessage.h"
#include "api/JsonWriter.h"
#include "api/SimulcastGroup.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineVideoStream.h"
#include "bridge/engine/SsrcRewrite.h"
#include "logger/Logger.h"
#include "math/helpers.h"
#include "memory/PartialSortExtractor.h"
#include "utils/ScopedInvariantChecker.h"
#include "utils/ScopedReentrancyBlocker.h"
namespace bridge
{

const float ActiveMediaList::AudioParticipant::MIN_NOISE = 7;

ActiveMediaList::AudioParticipant::AudioParticipant(const char* id)
    : audioLevel(0.0),
      maxRecentLevel(0.0),
      noiseLevel(50.0),
      ptt(false),
      endpointId(id),
      isLocal(true)
{
}

ActiveMediaList::AudioParticipant::AudioParticipant(const char* id, float noiseLevel, float recentLevel)
    : audioLevel(recentLevel),
      maxRecentLevel(recentLevel),
      noiseLevel(noiseLevel),
      ptt(false),
      endpointId(id),
      isLocal(false)
{
    history.fill(noiseLevel);
}

void ActiveMediaList::AudioParticipant::onNewLevel(uint8_t level, uint64_t timestamp)
{
    audioLevel = level;
    if (level == 0)
    {
        maxRecentLevel = 0; // muted
    }
    else
    {
        maxRecentLevel = std::max(maxRecentLevel, static_cast<float>(level));
    }

    history.update(level, timestamp);
}

void ActiveMediaList::AudioParticipant::History::update(uint8_t level, uint64_t timestamp)
{
    _index = (_index + 1) % _levels.size();

    const auto removedItem = _levels[_index];
    _levels[_index] = level;

    _totalLevel += level;
    _totalLevel -= removedItem;

    if (level > 0)
    {
        ++_nonZeroLevels;
    }

    if (removedItem > 0)
    {
        --_nonZeroLevels;
    }

    _timestamp = timestamp;
}

void ActiveMediaList::AudioParticipant::History::fill(uint8_t level)
{
    for (auto& item : _levels)
    {
        item = level;
    }
    if (level > 0)
    {
        _nonZeroLevels = _levels.size();
    }
    else
    {
        _nonZeroLevels = 0;
    }

    _totalLevel = level * _levels.size();
}

ActiveMediaList::ActiveMediaList(size_t instanceId,
    const std::vector<uint32_t>& audioSsrcs,
    const std::vector<api::SimulcastGroup>& videoSsrcs,
    const uint32_t defaultLastN,
    uint32_t audioLastN,
    uint32_t activeTalkerSilenceThresholdDb)
    : _logId("ActiveMediaList", instanceId),
      _defaultLastN(defaultLastN),
      _maxActiveListSize(defaultLastN + 1),
      _audioLastN(audioLastN),
      _activeTalkerSilenceThresholdDb(math::clamp<uint32_t>(activeTalkerSilenceThresholdDb, 6, 60)),
      _maxSpeakers(audioSsrcs.size()),
      _audioParticipants(maxParticipants),
      _incomingAudioLevels(32768),
      _audioSsrcs(SsrcRewrite::ssrcArraySize * 2),
      _audioSsrcRewriteMap(SsrcRewrite::ssrcArraySize * 2),
      _dominantSpeaker(0),
      _nominatedSpeaker(0),
      _videoParticipants(videoSsrcs.empty() ? 0 : maxParticipants),
      _videoSsrcs(videoSsrcs.empty() ? 0 : SsrcRewrite::ssrcArraySize * 2),
      _videoFeedbackSsrcLookupMap(videoSsrcs.empty() ? 0 : SsrcRewrite::ssrcArraySize * 2),
      _videoSsrcRewriteMap(videoSsrcs.empty() ? 0 : SsrcRewrite::ssrcArraySize * 2),
      _reverseVideoSsrcRewriteMap(videoSsrcs.empty() ? 0 : SsrcRewrite::ssrcArraySize * 2),
      _activeVideoListLookupMap(videoSsrcs.empty() ? 0 : 32),
#if DEBUG
      _reentrancyCounter(0),
#endif
      _lastRunTimestamp(0),
      _dominationTimestamp(0),
      _ssrcMapRevision(0),
      _transactionCounter(audioSsrcs[0])
{
    assert(videoSsrcs.empty() || videoSsrcs.size() >= _maxActiveListSize + 2);
    assert(audioSsrcs.size() <= SsrcRewrite::ssrcArraySize);
    assert(videoSsrcs.size() <= SsrcRewrite::ssrcArraySize);

    for (const auto audioSsrc : audioSsrcs)
    {
        _audioSsrcs.push(audioSsrc);
    }

    _videoScreenShareSsrc = api::SsrcPair({0, 0});

    for (const auto& group : videoSsrcs)
    {
        if (group.size() == 1)
        {
            _videoScreenShareSsrc = group[0];
            _videoFeedbackSsrcLookupMap.emplace(group[0].main, group[0].feedback);
        }
        else
        {
            _videoSsrcs.push(group);
            for (auto& simulcastLevel : group)
            {
                _videoFeedbackSsrcLookupMap.emplace(simulcastLevel.main, simulcastLevel.feedback);
            }
        }
    }
}

bool ActiveMediaList::addBarbellAudioParticipant(const size_t endpointIdHash,
    const char* endpointId,
    float noiseLevel,
    float recentLevel)
{
#if DEBUG
    utils::ScopedReentrancyBlocker blocker(_reentrancyCounter);
    utils::ScopedInvariantChecker<ActiveMediaList> invariantChecker(*this);
#endif
    if (_audioParticipants.contains(endpointIdHash))
    {
        return false;
    }

    _audioParticipants.emplace(endpointIdHash, AudioParticipant(endpointId, noiseLevel, recentLevel));
    return onAudioParticipantAdded(endpointIdHash, endpointId);
}

bool ActiveMediaList::addAudioParticipant(const size_t endpointIdHash, const char* endpointId)
{
#if DEBUG
    utils::ScopedReentrancyBlocker blocker(_reentrancyCounter);
    utils::ScopedInvariantChecker<ActiveMediaList> invariantChecker(*this);
#endif
    if (_audioParticipants.contains(endpointIdHash))
    {
        return false;
    }

    _audioParticipants.emplace(endpointIdHash, AudioParticipant(endpointId));
    return onAudioParticipantAdded(endpointIdHash, endpointId);
}

bool ActiveMediaList::onAudioParticipantAdded(const size_t endpointIdHash, const char* endpointId)
{
    if (!_dominantSpeaker)
    {
        _dominantSpeaker = endpointIdHash;
    }

    uint32_t ssrc;
    if (!_audioSsrcs.pop(ssrc))
    {
        logger::info("new audio endpoint %s %zu, added to active audio list",
            _logId.c_str(),
            endpointId,
            endpointIdHash);
        return false;
    }

    logger::info("new audio endpoint %s %zu, entered active audio list, mapped ssrc -> %u",
        _logId.c_str(),
        endpointId,
        endpointIdHash,
        ssrc);

    _audioSsrcRewriteMap.emplace(endpointIdHash, ssrc);
    [[maybe_unused]] const bool pushResult = _activeAudioList.pushToHead(endpointIdHash);
    assert(pushResult);
    ++_ssrcMapRevision;
    return true;
}

bool ActiveMediaList::removeAudioParticipant(const size_t endpointIdHash)
{
#if DEBUG
    utils::ScopedReentrancyBlocker blocker(_reentrancyCounter);
    utils::ScopedInvariantChecker<ActiveMediaList> invariantChecker(*this);
#endif

    _audioParticipants.erase(endpointIdHash);
    const auto audioSsrcRewriteMapItr = _audioSsrcRewriteMap.find(endpointIdHash);
    if (audioSsrcRewriteMapItr != _audioSsrcRewriteMap.end())
    {
        const auto ssrc = audioSsrcRewriteMapItr->second;
        _audioSsrcRewriteMap.erase(endpointIdHash);
        _audioSsrcs.push(ssrc);
        _activeAudioList.remove(endpointIdHash);
        ++_ssrcMapRevision;
        return true;
    }

    return false;
}

bool ActiveMediaList::addBarbellVideoParticipant(const size_t endpointIdHash,
    const SimulcastStream& simulcastStream,
    const utils::Optional<SimulcastStream>& secondarySimulcastStream,
    const char* endpointId)
{
#if DEBUG
    utils::ScopedReentrancyBlocker blocker(_reentrancyCounter);
    utils::ScopedInvariantChecker<ActiveMediaList> invariantChecker(*this);
#endif

    const auto videoParticipantsItr = _videoParticipants.find(endpointIdHash);
    if (videoParticipantsItr != _videoParticipants.end())
    {
        if (videoParticipantsItr->second.simulcastStream == simulcastStream &&
            videoParticipantsItr->second.secondarySimulcastStream == secondarySimulcastStream)
        {
            // Nothing has changed
            return false;
        }

        [[maybe_unused]] const bool wasRemoved = removeVideoParticipant(endpointIdHash);
        assert(wasRemoved);
    }

    _videoParticipants.emplace(endpointIdHash,
        VideoParticipant{endpointId, simulcastStream, secondarySimulcastStream, false});

    return onVideoParticipantAdded(endpointIdHash, simulcastStream, secondarySimulcastStream, endpointId);
}

bool ActiveMediaList::addVideoParticipant(const size_t endpointIdHash,
    const SimulcastStream& simulcastStream,
    const utils::Optional<SimulcastStream>& secondarySimulcastStream,
    const char* endpointId)
{
#if DEBUG
    utils::ScopedReentrancyBlocker blocker(_reentrancyCounter);
    utils::ScopedInvariantChecker<ActiveMediaList> invariantChecker(*this);
#endif

    const auto videoParticipantsItr = _videoParticipants.find(endpointIdHash);
    if (videoParticipantsItr != _videoParticipants.end())
    {
        return false;
    }
    _videoParticipants.emplace(endpointIdHash,
        VideoParticipant{endpointId, simulcastStream, secondarySimulcastStream, true});

    return onVideoParticipantAdded(endpointIdHash, simulcastStream, secondarySimulcastStream, endpointId);
}

bool ActiveMediaList::onVideoParticipantAdded(const size_t endpointIdHash,
    const SimulcastStream& simulcastStream,
    const utils::Optional<SimulcastStream>& secondarySimulcastStream,
    const char* endpointId)
{

    if (simulcastStream.isSendingSlides())
    {
        _videoScreenShareSsrcMapping.set(endpointIdHash,
            VideoScreenShareSsrcMapping{simulcastStream.levels[0].ssrc, _videoScreenShareSsrc.main});
        ++_ssrcMapRevision;
    }
    else if (secondarySimulcastStream.isSet() && secondarySimulcastStream.get().isSendingSlides())
    {
        _videoScreenShareSsrcMapping.set(endpointIdHash,
            VideoScreenShareSsrcMapping{secondarySimulcastStream.get().levels[0].ssrc, _videoScreenShareSsrc.main});
        ++_ssrcMapRevision;
    }

    if (_activeVideoListLookupMap.size() == _maxActiveListSize)
    {
        return endpointIdHash != _dominantSpeaker || updateActiveVideoList(_dominantSpeaker);
    }

    if (simulcastStream.isSendingVideo() ||
        (secondarySimulcastStream.isSet() && secondarySimulcastStream.get().isSendingVideo()))
    {
        api::SimulcastGroup simulcastGroup;
        if (!_videoSsrcs.pop(simulcastGroup))
        {
            assert(false);
            return false;
        }

        const uint32_t ssrc = secondarySimulcastStream.isSet() && secondarySimulcastStream.get().isSendingVideo()
            ? secondarySimulcastStream.get().levels[0].ssrc
            : simulcastStream.levels[0].ssrc;

        addToVideoRewriteMap(endpointIdHash, simulcastGroup);
        logger::info("%s %zu video mapped %u -> %u (%u %u)",
            _logId.c_str(),
            endpointId,
            endpointIdHash,
            ssrc,
            simulcastGroup[0].main,
            simulcastGroup[1].main,
            simulcastGroup[2].main);
    }

    [[maybe_unused]] const bool pushResult = _activeVideoList.pushToHead(endpointIdHash);
    logger::info("new video endpoint %s %zu, added to active video list, original ssrc %u",
        _logId.c_str(),
        endpointId,
        endpointIdHash,
        simulcastStream.levels[0].ssrc);
    _activeVideoListLookupMap.emplace(endpointIdHash, _activeVideoList.head());
    assert(pushResult);
    return endpointIdHash != _dominantSpeaker || updateActiveVideoList(_dominantSpeaker);
}

bool ActiveMediaList::removeVideoParticipant(const size_t endpointIdHash)
{
#if DEBUG
    utils::ScopedReentrancyBlocker blocker(_reentrancyCounter);
    utils::ScopedInvariantChecker<ActiveMediaList> invariantChecker(*this);
#endif

    if (!_videoParticipants.contains(endpointIdHash))
    {
        return false;
    }

    _videoParticipants.erase(endpointIdHash);
    if (_videoScreenShareSsrcMapping.isSet() && _videoScreenShareSsrcMapping.get().first == endpointIdHash)
    {
        _videoScreenShareSsrcMapping.clear();
        ++_ssrcMapRevision;
    }

    removeFromRewriteMap(endpointIdHash);

    _activeVideoList.remove(endpointIdHash);
    _activeVideoListLookupMap.erase(endpointIdHash);

    return true;
}

// note that zero level is mainly produced by muted participants. All unmuted produce non zero level.
void ActiveMediaList::updateLevels(const uint64_t timestamp)
{
    for (auto& audioParticipantEntry : _audioParticipants)
    {
        auto& audioParticipant = audioParticipantEntry.second;
        // Decay old max level over time (assuming process function called on average every 10ms)
        if (audioParticipant.maxRecentLevel > audioParticipant.noiseLevel)
        {
            audioParticipant.maxRecentLevel -=
                (audioParticipant.maxRecentLevel - audioParticipant.noiseLevel) * AudioParticipant::MAX_LEVEL_DECAY;
        }

        const bool audioOutage =
            utils::Time::diffGT(audioParticipant.history.getUpdateTime(), timestamp, utils::Time::ms * 200);
        if (audioParticipant.history.allNonZero() && !audioOutage)
        {
            // Move old min level over time towards mean about 3dB per 3 seconds
            audioParticipant.noiseLevel = std::max(audioParticipant.noiseLevel + AudioParticipant::NOISE_RAMPUP,
                static_cast<float>(AudioParticipant::MIN_NOISE));
        }

        if (audioOutage)
        {
            audioParticipant.maxRecentLevel = 0; // assume muted
            audioParticipant.audioLevel = 0;
        }
    }

    for (AudioLevelEntry levelEntry; _incomingAudioLevels.pop(levelEntry);)
    {
        auto participantLevelItr = _audioParticipants.find(levelEntry.participant);
        if (participantLevelItr == _audioParticipants.end())
        {
            continue;
        }

        auto& audioParticipant = participantLevelItr->second;
        audioParticipant.ptt = levelEntry.ptt;
        audioParticipant.onNewLevel(levelEntry.level, timestamp);

        if (audioParticipant.history.allNonZero())
        {
            audioParticipant.noiseLevel = std::min(audioParticipant.noiseLevel, audioParticipant.history.average());
        }
    }
}

// recently unmuted participants have some advantage because the score is higher as the
// noise level is likely lower than unmuted.
size_t ActiveMediaList::rankSpeakers()
{
    size_t speakerCount = 0;
    for (auto& audioParticipantEntry : _audioParticipants)
    {
        const auto& audioParticipant = audioParticipantEntry.second;
        if (audioParticipant.maxRecentLevel == 0)
        {
            continue; // muted
        }

        const float participantScore = audioParticipant.getScore();

        _highestScoringSpeakers[speakerCount++] = AudioParticipantScore{audioParticipantEntry.first,
            participantScore,
            std::max(0.0f, audioParticipant.noiseLevel)};
    }

    return speakerCount;
}

void ActiveMediaList::logAudioList()
{
    if (logger::_logLevel < logger::Level::DBG)
    {
        return;
    }

    utils::StringBuilder<1024> sb;
    size_t count = _maxSpeakers;
    for (auto* tail = _activeAudioList.tail(); count > 0 && tail != nullptr; tail = tail->_previous)
    {
        const auto entry = _audioParticipants.find(tail->_data);
        if (entry != _audioParticipants.end())
        {
            const auto& participant = entry->second;
            const auto* ssrc = _audioSsrcRewriteMap.getItem(entry->first);
            if (ssrc)
            {

                sb.append(entry->first).append(" -> ").append(*ssrc);
                const auto score = std::max(0.0f, participant.maxRecentLevel - participant.noiseLevel);
                sb.append(" ").append(participant.maxRecentLevel).append(" (").append(score).append(")\n");
            }
        }
        --count;
    }
    logger::debug("audio list updated\n%s", _logId.c_str(), sb.get());
}

// Algorithm for video switching:
// 1. Allow switching at most once per two second
// 2. Calculate average of (dBov + 127) level over time period for last 2s window and last
//    ~100ms window
// 3. Keep track of peak value (_maxRecentLevel) for each participant
// 4. Keep track of noise level (_noiseLevel) for each participant where _noiseLevel value is
//    the minimum level seen recently in the ~100ms window
// 5. Peak is decayed towards average of the 2s Window if no new max is received
// 6. Noise level estimate is increased if no new minimum is found
//
// Score is calculated as diff (spread) between _maxRecentLevel and _noiseLevel.
// To take over the dominant speaker position a participant has to have the highest score
// three times in a row. Current dominant speaker score must also be < 75% of new dominant speaker score. That is 33%
// louder over the entire measurement window. The time passed since last speaker switch must be > 2s.
//
// Active talkers are updated either via 'isPtt' flag (C9 conference case), or by processing highest ranking
// participants and checking against noise-level-based threshold.
void ActiveMediaList::process(const uint64_t timestamp,
    bool& outDominantSpeakerChanged,
    bool& outVideoMapChanged,
    bool& outAudioMapChanged)
{
#if DEBUG
    utils::ScopedReentrancyBlocker blocker(_reentrancyCounter);
#endif

    outDominantSpeakerChanged = false;
    outVideoMapChanged = false;
    outAudioMapChanged = false;

    if (utils::Time::diffLT(_lastRunTimestamp, timestamp, INTERVAL_MS * utils::Time::ms))
    {
        return;
    }
    _lastRunTimestamp = timestamp;

    updateLevels(timestamp);

    size_t speakerCount = rankSpeakers();
    if (speakerCount == 0)
    {
        TActiveTalkersSnapshot activeTalkersSnapshot;
        _activeTalkerSnapshot.write(activeTalkersSnapshot);
        return;
    }

    const auto* dominantSpeaker = _audioParticipants.getItem(_dominantSpeaker);

    memory::PartialSortExtractor<AudioParticipantScore> heap(_highestScoringSpeakers.begin(),
        _highestScoringSpeakers.begin() + speakerCount);

    auto nominatedSpeaker = heap.top();

    TActiveTalkersSnapshot activeTalkersSnapshot;
    for (size_t i = 0; i < _audioLastN && !heap.empty(); ++i)
    {
        const auto& top = heap.top();
        outAudioMapChanged |= updateActiveAudioList(top.participant);
        auto const& curParticipant = _audioParticipants.find(top.participant);

        if (top.score > _activeTalkerSilenceThresholdDb && activeTalkersSnapshot.count < activeTalkersSnapshot.maxSize)
        {
            ActiveTalker talker = {top.participant,
                curParticipant != _audioParticipants.end() ? false : curParticipant->second.ptt,
                (uint8_t)top.score,
                (uint8_t)top.noiseLevel};
            activeTalkersSnapshot.activeTalker[activeTalkersSnapshot.count++] = talker;
        }
        heap.pop();
    }
    _activeTalkerSnapshot.write(activeTalkersSnapshot);

    if (outAudioMapChanged && logger::_logLevel >= logger::Level::DBG)
    {
        logAudioList();
    }

    if (nominatedSpeaker.score == 0.0)
    {
        return;
    }

    if (nominatedSpeaker.participant != _nominatedSpeaker)
    {
        _nominationTimestamp = timestamp;
        _nominatedSpeaker = nominatedSpeaker.participant;
    }

    const float dominantSpeakerInstantScore = dominantSpeaker ? dominantSpeaker->getInstantScore() : 0.0;
    const float dominantSpeakerScore = dominantSpeaker ? dominantSpeaker->getScore() : 0.0;
    // switch at most every 2s
    // dominant must have stopped speaking or new speaker has persistently barged in for 2s
    if (nominatedSpeaker.participant != _dominantSpeaker &&
        utils::Time::diffGT(_dominationTimestamp, timestamp, minSpotlightDuration) &&
        (utils::Time::diffGT(_nominationTimestamp, timestamp, minSpotlightDuration) ||
            dominantSpeakerInstantScore < 3.0))
    {
        logger::info("process dominant speaker switch %lu (score %f) -> %lu (score %f)",
            _logId.c_str(),
            _dominantSpeaker.load(),
            dominantSpeakerScore,
            nominatedSpeaker.participant,
            nominatedSpeaker.score);

        _dominationTimestamp = timestamp;
        _dominantSpeaker = nominatedSpeaker.participant;
        outDominantSpeakerChanged = true;
        outVideoMapChanged = updateActiveVideoList(_dominantSpeaker);
    }
}

bool ActiveMediaList::updateActiveAudioList(const size_t endpointIdHash)
{
#if DEBUG
    utils::ScopedInvariantChecker<ActiveMediaList> invariantChecker(*this);
#endif

    if (_audioSsrcRewriteMap.end() != _audioSsrcRewriteMap.find(endpointIdHash))
    {
        if (!_activeAudioList.remove(endpointIdHash))
        {
            assert(false);
            return false;
        }
        [[maybe_unused]] const auto pushResult = _activeAudioList.pushToTail(endpointIdHash);
        assert(pushResult);
        return false; // content did not change
    }

    if (_audioSsrcRewriteMap.size() == _maxSpeakers)
    {
        size_t removedEndpointIdHash;
        if (!_activeAudioList.popFromHead(removedEndpointIdHash))
        {
            assert(false);
            return false;
        }

        const auto audioSsrcRewriteMapItr = _audioSsrcRewriteMap.find(removedEndpointIdHash);
        if (audioSsrcRewriteMapItr == _audioSsrcRewriteMap.end())
        {
            assert(false);
            return false;
        }

        const auto ssrc = audioSsrcRewriteMapItr->second;
        _audioSsrcRewriteMap.erase(removedEndpointIdHash);
        _audioSsrcs.push(ssrc);
    }

    uint32_t ssrc;
    if (!_audioSsrcs.pop(ssrc))
    {
        assert(false);
        return false;
    }

    _audioSsrcRewriteMap.emplace(endpointIdHash, ssrc);
    [[maybe_unused]] const bool pushResult = _activeAudioList.pushToTail(endpointIdHash);
    assert(pushResult);
    ++_ssrcMapRevision;
    return true;
}

bool ActiveMediaList::updateActiveVideoList(const size_t endpointIdHash)
{
#if DEBUG
    utils::ScopedInvariantChecker<ActiveMediaList> invariantChecker(*this);
#endif

    const auto videoParticipantsItr = _videoParticipants.find(endpointIdHash);
    if (videoParticipantsItr == _videoParticipants.end())
    {
        return false;
    }
    const auto& videoParticipant = videoParticipantsItr->second;

    {
        auto videoListEntry = _activeVideoList.tail();
        while (videoListEntry)
        {
            if (videoListEntry->_data == endpointIdHash)
            {
                _activeVideoList.remove(endpointIdHash);
                _activeVideoList.pushToTail(endpointIdHash);
                _activeVideoListLookupMap.erase(endpointIdHash);
                _activeVideoListLookupMap.emplace(endpointIdHash, _activeVideoList.tail());
                return true;
            }
            videoListEntry = videoListEntry->_previous;
        }
    }

    size_t removedEndpointIdHash = 0;
    if (_activeVideoListLookupMap.size() == _maxActiveListSize)
    {
        if (!_activeVideoList.popFromHead(removedEndpointIdHash))
        {
            assert(false);
            return false;
        }
        _activeVideoListLookupMap.erase(removedEndpointIdHash);
    }

    if (removedEndpointIdHash)
    {
        removeFromRewriteMap(removedEndpointIdHash);
    }

    if (videoParticipant.simulcastStream.isSendingVideo() ||
        (videoParticipant.secondarySimulcastStream.isSet() &&
            videoParticipant.secondarySimulcastStream.get().isSendingVideo()))
    {
        api::SimulcastGroup simulcastGroup;
        if (!_videoSsrcs.pop(simulcastGroup))
        {
            assert(false);
            return false;
        }

        addToVideoRewriteMap(endpointIdHash, simulcastGroup);
    }

    [[maybe_unused]] const bool addResult = _activeVideoList.pushToTail(endpointIdHash);
    assert(addResult);
    _activeVideoListLookupMap.emplace(endpointIdHash, _activeVideoList.tail());
    return true;
}

bool ActiveMediaList::makeLastNListMessage(const size_t lastN,
    const size_t endpointIdHash,
    const size_t pinTargetEndpointIdHash,
    utils::StringBuilder<1024>& outMessage)
{
    if (lastN > _defaultLastN || lastN == 0)
    {
        assert(false);
        return false;
    }

    api::DataChannelMessage::makeLastNStart(outMessage);
    bool isFirstElement = true;
    size_t i = 0;

    if (pinTargetEndpointIdHash)
    {
        const auto* videoParticipant = _videoParticipants.getItem(pinTargetEndpointIdHash);
        if (videoParticipant)
        {
            api::DataChannelMessage::makeLastNAppend(outMessage, videoParticipant->endpointId.c_str(), isFirstElement);
            isFirstElement = false;
            ++i;
        }
    }

    auto participantEntry = _activeVideoList.tail();
    while (participantEntry && i < lastN)
    {
        if (participantEntry->_data != pinTargetEndpointIdHash && participantEntry->_data != endpointIdHash)
        {
            const auto* videoParticipant = _videoParticipants.getItem(participantEntry->_data);
            if (videoParticipant)
            {
                api::DataChannelMessage::makeLastNAppend(outMessage,
                    videoParticipant->endpointId.c_str(),
                    isFirstElement);
                isFirstElement = false;
            }
            ++i;
        }
        participantEntry = participantEntry->_previous;
    }

    api::DataChannelMessage::makeLastNEnd(outMessage);
    return true;
}

bool ActiveMediaList::makeUserMediaMapMessage(const size_t lastN,
    const size_t endpointIdHash,
    const size_t pinTargetEndpointIdHash,
    const concurrency::MpmcHashmap32<size_t, EngineVideoStream*>& engineVideoStreams,
    utils::StringBuilder<1024>& outMessage)
{
    if (lastN > _defaultLastN || lastN == 0)
    {
        assert(false);
        return false;
    }

    api::DataChannelMessage::addUserMediaMapStart(outMessage);
    size_t addedElements = 0;

    const auto isPinTargetInActiveVideoList = isInUserActiveVideoList(pinTargetEndpointIdHash);

    if (pinTargetEndpointIdHash && !isPinTargetInActiveVideoList)
    {
        const auto videoStreamItr = engineVideoStreams.find(endpointIdHash);
        const auto targetVideoStreamItr = engineVideoStreams.find(pinTargetEndpointIdHash);
        if (videoStreamItr != engineVideoStreams.end() && targetVideoStreamItr != engineVideoStreams.end() &&
            videoStreamItr->second->pinSsrc.isSet())
        {
            const auto videoStream = videoStreamItr->second;
            const auto targetVideoStream = targetVideoStreamItr->second;

            api::DataChannelMessage::addUserMediaEndpointStart(outMessage,
                targetVideoStreamItr->second->endpointId.c_str());

            if (targetVideoStream->simulcastStream.isSendingVideo() ||
                (targetVideoStream->secondarySimulcastStream.isSet() &&
                    targetVideoStream->secondarySimulcastStream.get().isSendingVideo()))
            {
                api::DataChannelMessage::addUserMediaSsrc(outMessage, videoStream->pinSsrc.get().ssrc);
            }

            if (_videoScreenShareSsrcMapping.isSet() &&
                _videoScreenShareSsrcMapping.get().first == pinTargetEndpointIdHash)
            {
                api::DataChannelMessage::addUserMediaSsrc(outMessage,
                    _videoScreenShareSsrcMapping.get().second.rewriteSsrc);
            }

            api::DataChannelMessage::addUserMediaEndpointEnd(outMessage);
            ++addedElements;
        }
    }

    for (auto videoListEntry = _activeVideoList.tail(); videoListEntry && addedElements < lastN;
        videoListEntry = videoListEntry->_previous)
    {
        if (videoListEntry->_data == endpointIdHash ||
            (videoListEntry->_data == pinTargetEndpointIdHash && !isPinTargetInActiveVideoList))
        {
            continue;
        }

        const auto videoEndpointIdhash = videoListEntry->_data;
        const auto* videoParticipant = _videoParticipants.getItem(videoEndpointIdhash);
        if (!videoParticipant)
        {
            continue;
        }

        const char* endpointId = videoParticipant->endpointId.c_str();

        api::DataChannelMessage::addUserMediaEndpointStart(outMessage, endpointId);

        const auto rewriteMapItr = _videoSsrcRewriteMap.find(videoEndpointIdhash);
        if (rewriteMapItr != _videoSsrcRewriteMap.end())
        {
            api::DataChannelMessage::addUserMediaSsrc(outMessage, rewriteMapItr->second[0].main);
        }

        if (_videoScreenShareSsrcMapping.isSet() && _videoScreenShareSsrcMapping.get().first == videoEndpointIdhash)
        {
            api::DataChannelMessage::addUserMediaSsrc(outMessage,
                _videoScreenShareSsrcMapping.get().second.rewriteSsrc);
        }

        api::DataChannelMessage::addUserMediaEndpointEnd(outMessage);
        ++addedElements;
    }

    api::DataChannelMessage::addUserMediaMapEnd(outMessage);
    return true;
}

void ActiveMediaList::makeDominantSpeakerMessage(utils::StringBuilder<256>& outMessage)
{
    if (_dominantSpeaker == 0)
    {
        return;
    }

    auto* participant = _audioParticipants.getItem(_dominantSpeaker);
    if (participant)
    {
        api::DataChannelMessage::makeDominantSpeaker(outMessage, participant->endpointId.c_str());
    }
}

const std::map<size_t, ActiveTalker> ActiveMediaList::getActiveTalkers() const
{
    std::map<size_t, ActiveTalker> result;
    ActiveTalkersSnapshot<maxParticipants / 2> snapshot;
    _activeTalkerSnapshot.read(snapshot);
    for (size_t i = 0; i < snapshot.count && i < snapshot.maxSize; i++)
    {
        result.emplace(snapshot.activeTalker[i].endpointHashId, snapshot.activeTalker[i]);
    }
    return result;
}

bool ActiveMediaList::makeBarbellUserMediaMapMessage(utils::StringBuilder<1024>& outMessage,
    const engine::EndpointMembershipsMap& neighbourMembershipMap,
    bool includeVideo)
{
    auto umm = json::writer::createObjectWriter(outMessage);
    umm.addProperty("type", "user-media-map");
    umm.addProperty("msg-id", ++_transactionCounter);

    bool slidesAdded = false;
    if (includeVideo && (!_videoSsrcRewriteMap.empty() || _videoScreenShareSsrcMapping.isSet()))
    {
        auto videoArray = json::writer::createArrayWriter(outMessage, "video-endpoints");
        for (auto& item : _videoSsrcRewriteMap)
        {
            const auto* videoStream = _videoParticipants.getItem(item.first);
            if (videoStream && videoStream->isLocal)
            {
                auto videoEp = json::writer::createObjectWriter(outMessage);
                videoEp.addProperty("endpoint-id", videoStream->endpointId.c_str());

                auto videoSsrcs = json::writer::createArrayWriter(outMessage, "ssrcs");
                videoSsrcs.addElement(item.second[0].main);

                if (_videoScreenShareSsrcMapping.isSet() && _videoScreenShareSsrcMapping.get().first == item.first)
                {
                    slidesAdded = true;
                    videoSsrcs.addElement(_videoScreenShareSsrcMapping.get().second.rewriteSsrc);
                }
            }
        }
        if (!slidesAdded && _videoScreenShareSsrcMapping.isSet())
        {
            const auto* videoStream = _videoParticipants.getItem(_videoScreenShareSsrcMapping.get().first);
            if (videoStream && videoStream->isLocal)
            {
                auto videoEp = json::writer::createObjectWriter(outMessage);
                videoEp.addProperty("endpoint-id", videoStream->endpointId.c_str());
                auto videoSsrcs = json::writer::createArrayWriter(outMessage, "ssrcs");
                videoSsrcs.addElement(_videoScreenShareSsrcMapping.get().second.rewriteSsrc);
            }
        }
    }

    if (!_audioSsrcRewriteMap.empty())
    {
        auto audioArray = json::writer::createArrayWriter(outMessage, "audio-endpoints");
        for (auto& item : _audioSsrcRewriteMap)
        {
            auto* audioStream = _audioParticipants.getItem(item.first);
            if (audioStream && audioStream->isLocal)
            {
                auto audioEndpoint = json::writer::createObjectWriter(outMessage);
                audioEndpoint.addProperty("endpoint-id", audioStream->endpointId.c_str());
                {
                    auto audioSsrcs = json::writer::createArrayWriter(outMessage, "ssrcs");
                    audioSsrcs.addElement(item.second);
                }

                audioEndpoint.addProperty("noise-level", audioStream->noiseLevel);
                audioEndpoint.addProperty("level", audioStream->maxRecentLevel);

                auto* neighbours = neighbourMembershipMap.getItem(item.first);
                if (neighbours && !neighbours->memberships.empty())
                {
                    auto neighboursJson = json::writer::createArrayWriter(outMessage, "neighbours");
                    for (auto& n : neighbours->memberships)
                    {
                        neighboursJson.addElement(n);
                    }
                }
            }
        }
    }

    return true;
}

void ActiveMediaList::addToVideoRewriteMap(size_t endpointIdHash, api::SimulcastGroup simulcastGroup)
{
    logger::debug("add to video ssrcmap %zu", _logId.c_str(), endpointIdHash);
    _videoSsrcRewriteMap.emplace(endpointIdHash, simulcastGroup);
    for (auto& ssrcPair : simulcastGroup)
    {
        _reverseVideoSsrcRewriteMap.emplace(ssrcPair.main, endpointIdHash);
    }
    ++_ssrcMapRevision;
}

void ActiveMediaList::removeFromRewriteMap(size_t endpointIdHash)
{
    logger::debug("remove from video ssrcmap %zu", _logId.c_str(), endpointIdHash);
    const auto rewriteMapItr = _videoSsrcRewriteMap.find(endpointIdHash);
    if (rewriteMapItr != _videoSsrcRewriteMap.end())
    {
        const auto simulcastGroup = rewriteMapItr->second;
        _videoSsrcRewriteMap.erase(endpointIdHash);
        for (auto& ssrcPair : simulcastGroup)
        {
            _reverseVideoSsrcRewriteMap.erase(ssrcPair.main);
        }
        _videoSsrcs.push(simulcastGroup);
    }
    ++_ssrcMapRevision;
}

#if DEBUG
void ActiveMediaList::checkInvariant()
{
    {
        auto audioListEntry = _activeAudioList.head();
        size_t count = 0;
        while (audioListEntry)
        {
            assert(_audioSsrcRewriteMap.contains(audioListEntry->_data));
            ++count;
            audioListEntry = audioListEntry->_next;
        }
        assert(count == _audioSsrcRewriteMap.size());
    }

    {
        auto videoListEntry = _activeVideoList.head();
        size_t count = 0;
        while (videoListEntry)
        {
            ++count;
            assert(_videoParticipants.contains(videoListEntry->_data));

            const auto activeVideoListLookupMapItr = _activeVideoListLookupMap.find(videoListEntry->_data);
            assert(activeVideoListLookupMapItr != _activeVideoListLookupMap.end());
            assert(activeVideoListLookupMapItr->second == videoListEntry);

            videoListEntry = videoListEntry->_next;
        }

        assert(_activeVideoListLookupMap.size() == count);
        assert(_videoSsrcRewriteMap.size() * 3 == _reverseVideoSsrcRewriteMap.size());

        for (const auto& videoSsrcRewriteMapEntry : _videoSsrcRewriteMap)
        {
            assert(_videoParticipants.contains(videoSsrcRewriteMapEntry.first));

            videoListEntry = _activeVideoList.head();
            bool found = false;
            while (videoListEntry)
            {
                if (videoListEntry->_data == videoSsrcRewriteMapEntry.first)
                {
                    found = true;
                }
                videoListEntry = videoListEntry->_next;
            }
            assert(found);
        }
    }
}
#endif

} // namespace bridge
