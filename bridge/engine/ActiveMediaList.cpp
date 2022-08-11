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

ActiveMediaList::AudioParticipant::AudioParticipant(const char* id, bool isLocal)
    : levels({0}),
      index(lengthShortWindow - 1),
      indexEndShortWindow(0),
      totalLevelLongWindow(0),
      totalLevelShortWindow(0),
      nonZeroLevelsShortWindow(0),
      maxRecentLevel(0.0),
      noiseLevel(50.0),
      ptt(false),
      endpointId(id),
      isLocal(isLocal)
{
    memset(levels.data(), 0, levels.size());
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
      _dominantSpeakerId(0),
      _prevWinningDominantSpeaker(0),
      _consecutiveDominantSpeakerWins(0),
      _videoParticipants(maxParticipants),
      _videoSsrcs(SsrcRewrite::ssrcArraySize * 2),
      _videoFeedbackSsrcLookupMap(SsrcRewrite::ssrcArraySize * 2),
      _videoSsrcRewriteMap(SsrcRewrite::ssrcArraySize * 2),
      _reverseVideoSsrcRewriteMap(SsrcRewrite::ssrcArraySize * 2),
      _activeVideoListLookupMap(32),
#if DEBUG
      _reentrancyCounter(0),
#endif
      _lastRunTimestamp(0),
      _lastChangeTimestamp(0),
      _ssrcMapRevision(0)
{
    assert(videoSsrcs.size() >= _maxActiveListSize + 2);
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

bool ActiveMediaList::addBarbellAudioParticipant(const size_t endpointIdHash, const char* endpointId)
{
#if DEBUG
    utils::ScopedReentrancyBlocker blocker(_reentrancyCounter);
    utils::ScopedInvariantChecker<ActiveMediaList> invariantChecker(*this);
#endif
    const auto audioParticipantsItr = _audioParticipants.find(endpointIdHash);
    if (audioParticipantsItr != _audioParticipants.end())
    {
        return false;
    }

    _audioParticipants.emplace(endpointIdHash, AudioParticipant(endpointId, false));
    return onAudioParticipantAdded(endpointIdHash, endpointId);
}

bool ActiveMediaList::addAudioParticipant(const size_t endpointIdHash, const char* endpointId)
{
#if DEBUG
    utils::ScopedReentrancyBlocker blocker(_reentrancyCounter);
    utils::ScopedInvariantChecker<ActiveMediaList> invariantChecker(*this);
#endif

    const auto audioParticipantsItr = _audioParticipants.find(endpointIdHash);
    if (audioParticipantsItr != _audioParticipants.end())
    {
        return false;
    }

    _audioParticipants.emplace(endpointIdHash, AudioParticipant(endpointId, true));
    return onAudioParticipantAdded(endpointIdHash, endpointId);
}

bool ActiveMediaList::onAudioParticipantAdded(const size_t endpointIdHash, const char* endpointId)
{
    if (!_dominantSpeakerId)
    {
        _dominantSpeakerId = endpointIdHash;
    }

    uint32_t ssrc;
    if (!_audioSsrcs.pop(ssrc))
    {
        logger::info("new endpoint %s %zu, added to active audio list", _logId.c_str(), endpointId, endpointIdHash);
        return false;
    }

    logger::info("new endpoint %s %zu, added to active audio list, mapped ssrc %u",
        _logId.c_str(),
        endpointId,
        endpointIdHash,
        ssrc);

    _audioSsrcRewriteMap.emplace(endpointIdHash, ssrc);
    const bool pushResult = _activeAudioList.pushToHead(endpointIdHash);
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
        return false;
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
    }
    else if (secondarySimulcastStream.isSet() && secondarySimulcastStream.get().isSendingSlides())
    {
        _videoScreenShareSsrcMapping.set(endpointIdHash,
            VideoScreenShareSsrcMapping{secondarySimulcastStream.get().levels[0].ssrc, _videoScreenShareSsrc.main});
    }

    if (_activeVideoListLookupMap.size() == _maxActiveListSize)
    {
        return endpointIdHash != _dominantSpeakerId || updateActiveVideoList(_dominantSpeakerId);
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

        addToRewriteMap(endpointIdHash, simulcastGroup);
        logger::debug("%s %zu video mapped to %u", _logId.c_str(), endpointId, endpointIdHash, simulcastGroup[0].main);
    }

    const bool pushResult = _activeVideoList.pushToHead(endpointIdHash);
    logger::info("new endpoint %s %zu, added to active video list, original ssrc %u",
        _logId.c_str(),
        endpointId,
        endpointIdHash,
        simulcastStream.levels[0].ssrc);
    _activeVideoListLookupMap.emplace(endpointIdHash, _activeVideoList.head());
    assert(pushResult);
    return endpointIdHash != _dominantSpeakerId || updateActiveVideoList(_dominantSpeakerId);
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
    }

    removeFromRewriteMap(endpointIdHash);

    _activeVideoList.remove(endpointIdHash);
    _activeVideoListLookupMap.erase(endpointIdHash);

    return true;
}

// note that  zero level is mainly produced by muted participants. All unmuted produce non zero level.
// nonZerolevelWindow thus means how long a participant has been unmuted.
void ActiveMediaList::updateLevels(const uint64_t timestamp)
{
    for (auto& participantLevelEntry : _audioParticipants)
    {
        auto& participantLevels = participantLevelEntry.second;
        // Decay old max level over time (assuming process function called on average every 10ms)
        // participantLevels.maxRecentLevel *= AudioParticipant::MAX_LEVEL_DECAY;
        float averageLevelLongWindow =
            static_cast<float>(participantLevels.totalLevelLongWindow) / participantLevels.levels.size();
        participantLevels.maxRecentLevel -=
            (participantLevels.maxRecentLevel - averageLevelLongWindow) * AudioParticipant::MAX_LEVEL_DECAY;
        // Move old min level over time towards mean about 3dB per 3 seconds
        participantLevels.noiseLevel = participantLevels.noiseLevel + AudioParticipant::NOISE_RAMPUP;
        participantLevels.noiseLevel =
            std::max(participantLevels.noiseLevel, static_cast<float>(AudioParticipant::MIN_NOISE));
    }

    for (AudioLevelEntry levelEntry; _incomingAudioLevels.pop(levelEntry);)
    {
        auto participantLevelItr = _audioParticipants.find(levelEntry.participant);
        if (participantLevelItr == _audioParticipants.end())
        {
            continue;
        }

        auto& participantLevels = participantLevelItr->second;
        participantLevels.ptt = levelEntry.ptt;

        // Update the energy history

        participantLevels.index = (participantLevels.index + 1) % participantLevels.levels.size();
        uint8_t levelLeavingLongWindow = participantLevels.levels[participantLevels.index];
        uint8_t levelLeavingShortWindow = participantLevels.levels[participantLevels.indexEndShortWindow];
        participantLevels.indexEndShortWindow =
            (participantLevels.indexEndShortWindow + 1) % participantLevels.levels.size();
        participantLevels.levels[participantLevels.index] = levelEntry.level;

        // Update average level, max level, min level and number of non zero entries

        participantLevels.totalLevelLongWindow += (levelEntry.level - levelLeavingLongWindow);
        participantLevels.totalLevelShortWindow += (levelEntry.level - levelLeavingShortWindow);

        participantLevels.maxRecentLevel =
            std::max(participantLevels.maxRecentLevel, static_cast<float>(levelEntry.level));

        if (participantLevels.ptt)
        {
            participantLevels.noiseLevel = 37;
        }
        else if (levelEntry.level != 0 && participantLevels.nonZeroLevelsShortWindow == lengthShortWindow)
        {
            participantLevels.noiseLevel = std::min(participantLevels.noiseLevel,
                static_cast<float>(participantLevels.totalLevelShortWindow) / lengthShortWindow);
        }

        if (levelLeavingShortWindow != 0)
        {
            participantLevels.nonZeroLevelsShortWindow--;
        }

        if (levelEntry.level != 0)
        {
            participantLevels.nonZeroLevelsShortWindow++;
        }
    }
}

// recently unmuted participants have some advantage because the score is higher as the
// noise level is likely lower than unmuted.
size_t ActiveMediaList::rankSpeakers(float& currentDominantSpeakerScore)
{
    currentDominantSpeakerScore = 0;

    size_t speakerCount = 0;
    for (auto& participantLevelEntry : _audioParticipants)
    {
        const auto& participantLevels = participantLevelEntry.second;
        if (participantLevels.maxRecentLevel == 0)
        {
            continue;
        }

        const float participantScore = std::max(0.0f, participantLevels.maxRecentLevel - participantLevels.noiseLevel);

        if (participantLevelEntry.first == _dominantSpeakerId)
        {
            currentDominantSpeakerScore = participantScore;
        }

        _highestScoringSpeakers[speakerCount++] = AudioParticipantScore{participantLevelEntry.first,
            participantScore,
            std::max(0.0f, participantLevels.noiseLevel)};
    }

    return speakerCount;
}

// Algorithm for video switching:
// 1. Allow switching at most once per two second
// 2. Calculate average of (dBov + 127) level over time period for last 2s window and last
//    ~100ms window
// 3. Keep track of peak value (_maxRecentLevel) for each participant
// 4. Keep track of noise level (_noiseLevel) for each participant where _noiseLevel value is
//    the minimum level seen recently in the ~100ms window
// 5. Peak is decayed towards average of the 2s Window if no new max is recevied
// 6. Noise level estimate is increased if no new minimum is found
//
// Score is calcuated as diff (spread) between _maxRecentLevel and _noiseLevel.
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

    float currentDominantSpeakerScore = 0.0;
    size_t speakerCount = rankSpeakers(currentDominantSpeakerScore);
    if (speakerCount == 0)
    {
        return;
    }

    memory::PartialSortExtractor<AudioParticipantScore> heap(_highestScoringSpeakers.begin(),
        _highestScoringSpeakers.begin() + speakerCount);

    auto dominantSpeaker = heap.top();

    TActiveTalkersSnapshot activeTalkersSnapshot;
    for (size_t i = 0; i < _audioLastN && !heap.empty(); ++i)
    {
        const auto& top = heap.top();
        outAudioMapChanged = updateActiveAudioList(top.participant);
        auto const& curParticipant = _audioParticipants.find(top.participant);

        if (top.score - top.noiseLevel > _activeTalkerSilenceThresholdDb)
        {
            if (activeTalkersSnapshot.count < activeTalkersSnapshot.maxSize)
            {
                ActiveTalker talker = {top.participant,
                    curParticipant != _audioParticipants.end() ? false : curParticipant->second.ptt,
                    (uint8_t)top.score,
                    (uint8_t)top.noiseLevel};
                activeTalkersSnapshot.activeTalker[activeTalkersSnapshot.count++] = talker;
            }
        }
        heap.pop();
    }
    _activeTalkerSnapshot.write(activeTalkersSnapshot);

    if (timestamp - _lastChangeTimestamp + 10 * utils::Time::ms * (requiredConsecutiveWins - 1) <
        maxSwitchDominantSpeakerEvery)
    {
        return;
    }

    // nominate dominant speaker (for video)
    if (dominantSpeaker.participant == _prevWinningDominantSpeaker)
    {
        _consecutiveDominantSpeakerWins++;
    }
    else
    {
        _consecutiveDominantSpeakerWins = 1;
        _prevWinningDominantSpeaker = dominantSpeaker.participant;
    }

    _lastRunTimestamp = timestamp;
    if (dominantSpeaker.participant != _dominantSpeakerId &&
        ((!_dominantSpeakerId || currentDominantSpeakerScore < 0.01) ||
            (_consecutiveDominantSpeakerWins >= requiredConsecutiveWins &&
                currentDominantSpeakerScore < 0.75 * dominantSpeaker.score &&
                timestamp - _lastChangeTimestamp >= maxSwitchDominantSpeakerEvery)))
    {
        logger::info("process dominant speaker switch %lu (score %f) -> %lu (score %f)",
            _logId.c_str(),
            _dominantSpeakerId.load(),
            currentDominantSpeakerScore,
            dominantSpeaker.participant,
            dominantSpeaker.score);

        _lastChangeTimestamp = timestamp;
        _dominantSpeakerId = dominantSpeaker.participant;
        outDominantSpeakerChanged = true;
        outVideoMapChanged = updateActiveVideoList(_dominantSpeakerId);
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
        const auto pushResult = _activeAudioList.pushToTail(endpointIdHash);
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
    const bool pushResult = _activeAudioList.pushToTail(endpointIdHash);
    assert(pushResult);

    logger::debug("endpointIdHash %zu, ssrc %u added to active audio list", _logId.c_str(), endpointIdHash, ssrc);
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

        addToRewriteMap(endpointIdHash, simulcastGroup);
    }

    const bool addResult = _activeVideoList.pushToTail(endpointIdHash);
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
            videoStreamItr->second->_pinSsrc.isSet())
        {
            const auto videoStream = videoStreamItr->second;
            const auto targetVideoStream = targetVideoStreamItr->second;

            api::DataChannelMessage::addUserMediaEndpointStart(outMessage,
                targetVideoStreamItr->second->_endpointId.c_str());

            if (targetVideoStream->_simulcastStream.isSendingVideo() ||
                (targetVideoStream->_secondarySimulcastStream.isSet() &&
                    targetVideoStream->_secondarySimulcastStream.get().isSendingVideo()))
            {
                api::DataChannelMessage::addUserMediaSsrc(outMessage, videoStream->_pinSsrc.get().ssrc);
            }

            if (_videoScreenShareSsrcMapping.isSet() &&
                _videoScreenShareSsrcMapping.get().first == pinTargetEndpointIdHash)
            {
                api::DataChannelMessage::addUserMediaSsrc(outMessage,
                    _videoScreenShareSsrcMapping.get().second._rewriteSsrc);
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
        else
        {
            logger::warn("cannot find video ssrcmap for %zu", "", videoEndpointIdhash);
        }

        if (_videoScreenShareSsrcMapping.isSet() && _videoScreenShareSsrcMapping.get().first == videoEndpointIdhash)
        {
            api::DataChannelMessage::addUserMediaSsrc(outMessage,
                _videoScreenShareSsrcMapping.get().second._rewriteSsrc);
        }

        api::DataChannelMessage::addUserMediaEndpointEnd(outMessage);
        ++addedElements;
    }

    api::DataChannelMessage::addUserMediaMapEnd(outMessage);
    return true;
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

bool ActiveMediaList::makeBarbellUserMediaMapMessage(utils::StringBuilder<1024>& outMessage)
{
    auto umm = json::writer::createObjectWriter(outMessage);
    umm.addProperty("type", "user-media-map");

    if (!_videoSsrcRewriteMap.empty())
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
                    videoSsrcs.addElement(_videoScreenShareSsrcMapping.get().second._rewriteSsrc);
                }
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
                auto audioSsrcs = json::writer::createArrayWriter(outMessage, "ssrcs");
                audioSsrcs.addElement(item.second);
            }
        }
    }

    return true;
}

void ActiveMediaList::addToRewriteMap(size_t endpointIdHash, api::SimulcastGroup simulcastGroup)
{
    logger::debug("add to ssrcmap %zu", "", endpointIdHash);
    _videoSsrcRewriteMap.emplace(endpointIdHash, simulcastGroup);
    for (auto& ssrcPair : simulcastGroup)
    {
        _reverseVideoSsrcRewriteMap.emplace(ssrcPair.main, endpointIdHash);
    }
    ++_ssrcMapRevision;
}

void ActiveMediaList::removeFromRewriteMap(size_t endpointIdHash)
{
    logger::debug("remove from ssrcmap %zu", "", endpointIdHash);
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
