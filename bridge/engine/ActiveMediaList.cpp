#include "bridge/engine/ActiveMediaList.h"
#include "api/DataChannelMessage.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineVideoStream.h"
#include "bridge/engine/SsrcRewrite.h"
#include "logger/Logger.h"
#include "utils/ScopedInvariantChecker.h"
#include "utils/ScopedReentrancyBlocker.h"

namespace bridge
{

ActiveMediaList::AudioParticipant::AudioParticipant()
    : _levels({0}),
      _index(lengthShortWindow - 1),
      _indexEndShortWindow(0),
      _totalLevelLongWindow(0),
      _totalLevelShortWindow(0),
      _nonZeroLevelsLongWindow(0),
      _nonZeroLevelsShortWindow(0),
      _maxRecentLevel(0.0),
      _noiseLevel(50.0)
{
    memset(_levels.data(), 0, _levels.size());
}

ActiveMediaList::ActiveMediaList(const std::vector<uint32_t>& audioSsrcs,
    const std::vector<SimulcastLevel>& videoSsrcs,
    const uint32_t defaultLastN)
    : _defaultLastN(defaultLastN),
      _maxActiveListSize(defaultLastN + 1),
      _audioParticipants(maxParticipants),
      _incomingAudioLevels(32768),
      _audioSsrcs(SsrcRewrite::ssrcArraySize * 2),
      _audioSsrcRewriteMap(SsrcRewrite::ssrcArraySize * 2),
      _dominantSpeaker(0),
      _prevWinningDominantSpeaker(0),
      _highestScoringSpeaker(0),
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
      _lastRunTimestampMs(0),
      _lastChangeTimestampMs(0)
{
    assert(audioSsrcs.size() >= _maxActiveListSize);
    assert(videoSsrcs.size() >= _maxActiveListSize + 2);
    assert(audioSsrcs.size() <= SsrcRewrite::ssrcArraySize);
    assert(videoSsrcs.size() <= SsrcRewrite::ssrcArraySize);

    for (const auto audioSsrc : audioSsrcs)
    {
        _audioSsrcs.push(audioSsrc);
    }

    for (const auto videoSsrc : videoSsrcs)
    {
        _videoSsrcs.push(videoSsrc);
        _videoFeedbackSsrcLookupMap.emplace(videoSsrc._ssrc, videoSsrc._feedbackSsrc);
    }

    if (!_videoSsrcs.pop(_videoScreenShareSsrc))
    {
        _videoScreenShareSsrc = SimulcastLevel({0, 0, false});
        assert(false);
    }
}

bool ActiveMediaList::addAudioParticipant(const size_t endpointIdHash)
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

    _audioParticipants.emplace(endpointIdHash, AudioParticipant());
    if (!_dominantSpeaker)
    {
        _dominantSpeaker = endpointIdHash;
    }

    if (_audioSsrcRewriteMap.size() == _maxActiveListSize)
    {
        return false;
    }

    uint32_t ssrc;
    if (!_audioSsrcs.pop(ssrc))
    {
        assert(false);
        return false;
    }

    _audioSsrcRewriteMap.emplace(endpointIdHash, ssrc);
    const bool pushResult = _activeAudioList.pushToHead(endpointIdHash);
    assert(pushResult);
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
        return true;
    }

    return false;
}

bool ActiveMediaList::addVideoParticipant(const size_t endpointIdHash,
    const SimulcastStream& simulcastStream,
    const utils::Optional<SimulcastStream>& secondarySimulcastStream)
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

    _videoParticipants.emplace(endpointIdHash, VideoParticipant{simulcastStream, secondarySimulcastStream});

    if (simulcastStream.isSendingSlides())
    {
        if (!_videoScreenShareSsrcMapping.isSet())
        {
            _videoScreenShareSsrcMapping.set(endpointIdHash,
                VideoScreenShareSsrcMapping{simulcastStream._levels[0]._ssrc, _videoScreenShareSsrc._ssrc});
        }
    }
    else if (secondarySimulcastStream.isSet() && secondarySimulcastStream.get().isSendingSlides())
    {
        if (!_videoScreenShareSsrcMapping.isSet())
        {
            _videoScreenShareSsrcMapping.set(endpointIdHash,
                VideoScreenShareSsrcMapping{secondarySimulcastStream.get()._levels[0]._ssrc,
                    _videoScreenShareSsrc._ssrc});
        }
    }

    if (_activeVideoListLookupMap.size() == _maxActiveListSize)
    {
        return endpointIdHash != _dominantSpeaker || updateActiveVideoList(_dominantSpeaker);
    }

    if (simulcastStream.isSendingVideo() ||
        (secondarySimulcastStream.isSet() && secondarySimulcastStream.get().isSendingVideo()))
    {
        SimulcastLevel simulcastLevel;
        if (!_videoSsrcs.pop(simulcastLevel))
        {
            assert(false);
            return false;
        }

        _videoSsrcRewriteMap.emplace(endpointIdHash, simulcastLevel);
        _reverseVideoSsrcRewriteMap.emplace(simulcastLevel._ssrc, endpointIdHash);
    }

    const bool pushResult = _activeVideoList.pushToHead(endpointIdHash);
    _activeVideoListLookupMap.emplace(endpointIdHash, _activeVideoList.head());
    assert(pushResult);
    return endpointIdHash != _dominantSpeaker || updateActiveVideoList(_dominantSpeaker);
}

void ActiveMediaList::removeVideoParticipant(const size_t endpointIdHash)
{
#if DEBUG
    utils::ScopedReentrancyBlocker blocker(_reentrancyCounter);
    utils::ScopedInvariantChecker<ActiveMediaList> invariantChecker(*this);
#endif

    if (!_videoParticipants.contains(endpointIdHash))
    {
        return;
    }

    _videoParticipants.erase(endpointIdHash);
    if (_videoScreenShareSsrcMapping.isSet() && _videoScreenShareSsrcMapping.get().first == endpointIdHash)
    {
        _videoScreenShareSsrcMapping.clear();
    }

    const auto rewriteMapItr = _videoSsrcRewriteMap.find(endpointIdHash);
    if (rewriteMapItr != _videoSsrcRewriteMap.end())
    {
        const auto simulcastLevel = rewriteMapItr->second;
        _videoSsrcRewriteMap.erase(endpointIdHash);
        _reverseVideoSsrcRewriteMap.erase(simulcastLevel._ssrc);
        _videoSsrcs.push(simulcastLevel);
    }

    _activeVideoList.remove(endpointIdHash);
    _activeVideoListLookupMap.erase(endpointIdHash);
}

// Algorithm:
// 1. Allow switching at most once per two second
// 2. Calculate average of (dBov + 127) level over time period for last 2s window and last
//    ~100ms window
// 3. Keep track of peak value (_maxRecentLevel) for each participant
// 4. Keep track of noise level (_noiseLevel) for each participant where _noiseLevel value is
//    the minimum level seen recently in the ~100ms window
// 5. Peak is decayed towards average of the 2s Window if no new max is recevied
// 6. Noise level estimate is increased if no new minimum is found
//
// Score is calcuated as diff (spread) between _maxRecentLevel and _noiseLevel - somewhat reduced if
// user has been muted. To take over the dominant speaker position a participant has to have the highest score
// three times in a row
void ActiveMediaList::process(const uint64_t timestampMs, bool& outDominantSpeakerChanged, bool& outUserMediaMapChanged)
{
#if DEBUG
    utils::ScopedReentrancyBlocker blocker(_reentrancyCounter);
#endif

    outDominantSpeakerChanged = false;
    outUserMediaMapChanged = false;

    if ((timestampMs - _lastRunTimestampMs) < intervalMs)
    {
        return;
    }

    for (auto& participantLevelEntry : _audioParticipants)
    {
        auto& participantLevels = participantLevelEntry.second;
        // Decay old max level over time (assuming process function called on average every 10ms)
        // participantLevels._maxRecentLevel *= AudioParticipant::decayOfMaxLevel;
        float averageLevelLongWindow =
            static_cast<float>(participantLevels._totalLevelLongWindow) / participantLevels._levels.size();
        participantLevels._maxRecentLevel -=
            (participantLevels._maxRecentLevel - averageLevelLongWindow) * AudioParticipant::decayOfMaxLevel;
        // Move old min level over time towards mean about 3dB per 3 seconds
        participantLevels._noiseLevel = participantLevels._noiseLevel + AudioParticipant::noiseLevelRampup;
        participantLevels._noiseLevel =
            std::max(participantLevels._noiseLevel, static_cast<float>(AudioParticipant::minNoiseLevel));
    }

    if (_incomingAudioLevels.empty())
    {
        _lastRunTimestampMs = timestampMs;
        return;
    }

    for (AudioLevelEntry levelEntry; _incomingAudioLevels.pop(levelEntry);)
    {
        auto participantLevelItr = _audioParticipants.find(levelEntry._participant);
        if (participantLevelItr == _audioParticipants.end())
        {
            continue;
        }

        auto& participantLevels = participantLevelItr->second;

        // Update the energy history

        participantLevels._index = (participantLevels._index + 1) % participantLevels._levels.size();
        uint8_t levelLeavingLongWindow = participantLevels._levels[participantLevels._index];
        uint8_t levelLeavingShortWindow = participantLevels._levels[participantLevels._indexEndShortWindow];
        participantLevels._indexEndShortWindow =
            (participantLevels._indexEndShortWindow + 1) % participantLevels._levels.size();
        participantLevels._levels[participantLevels._index] = levelEntry._level;

        // Update average level, max level, min level and number of non zero entries

        participantLevels._totalLevelLongWindow += (levelEntry._level - levelLeavingLongWindow);
        participantLevels._totalLevelShortWindow += (levelEntry._level - levelLeavingShortWindow);

        participantLevels._maxRecentLevel =
            std::max(participantLevels._maxRecentLevel, static_cast<float>(levelEntry._level));
        if (levelEntry._level != 0 && participantLevels._nonZeroLevelsShortWindow == lengthShortWindow)
        {
            participantLevels._noiseLevel = std::min(participantLevels._noiseLevel,
                static_cast<float>(participantLevels._totalLevelShortWindow) / lengthShortWindow);
        }

        if (levelLeavingLongWindow != 0)
        {
            participantLevels._nonZeroLevelsLongWindow--;
        }
        if (levelLeavingShortWindow != 0)
        {
            participantLevels._nonZeroLevelsShortWindow--;
        }

        if (levelEntry._level != 0)
        {
            participantLevels._nonZeroLevelsLongWindow++;
            participantLevels._nonZeroLevelsShortWindow++;
        }
    }

    auto newDominantSpeaker = _dominantSpeaker;

    float maxDominantSpeakerScore = 0.0;
    float currentDominantSpeakerScore = 0.0;

    for (auto& participantLevelEntry : _audioParticipants)
    {
        auto& participantLevels = participantLevelEntry.second;
        const float spread = participantLevels._maxRecentLevel - participantLevels._noiseLevel;
        const float dominantSpeakerScore =
            spread * (static_cast<float>(participantLevels._nonZeroLevelsLongWindow) / numLevels);

        if (participantLevelEntry.first == _dominantSpeaker)
        {
            currentDominantSpeakerScore = dominantSpeakerScore;
        }

        if (dominantSpeakerScore > maxDominantSpeakerScore)
        {
            maxDominantSpeakerScore = dominantSpeakerScore;
            newDominantSpeaker = participantLevelEntry.first;
        }
    }

    if (_highestScoringSpeaker != newDominantSpeaker)
    {
        logger::info("process active speaker switch %zu", "ActiveMediaList", newDominantSpeaker);
        updateActiveAudioList(newDominantSpeaker);
        _highestScoringSpeaker = newDominantSpeaker;
    }

    if (timestampMs - _lastChangeTimestampMs + 10 * (requiredConsecutiveWins - 1) < maxSwitchDominantSpeakerEveryMs)
    {
        return;
    }

    if (newDominantSpeaker == _prevWinningDominantSpeaker)
    {
        _consecutiveDominantSpeakerWins++;
    }
    else
    {
        _consecutiveDominantSpeakerWins = 1;
        _prevWinningDominantSpeaker = newDominantSpeaker;
    }

    _lastRunTimestampMs = timestampMs;
    if (newDominantSpeaker != _dominantSpeaker &&
        ((!_dominantSpeaker || currentDominantSpeakerScore < 0.01) ||
            (_consecutiveDominantSpeakerWins >= requiredConsecutiveWins &&
                currentDominantSpeakerScore < 0.75 * maxDominantSpeakerScore &&
                timestampMs - _lastChangeTimestampMs >= maxSwitchDominantSpeakerEveryMs)))
    {
        logger::info("process dominant speaker switch %lu (score %f) -> %lu (score %f)",
            "ActiveMediaList",
            _dominantSpeaker,
            currentDominantSpeakerScore,
            newDominantSpeaker,
            maxDominantSpeakerScore);

        _lastChangeTimestampMs = timestampMs;
        _dominantSpeaker = newDominantSpeaker;
        outDominantSpeakerChanged = true;
        outUserMediaMapChanged = updateActiveVideoList(_dominantSpeaker);
    }
}

bool ActiveMediaList::updateActiveAudioList(const size_t endpointIdHash)
{
#if DEBUG
    utils::ScopedInvariantChecker<ActiveMediaList> invariantChecker(*this);
#endif

    if (!endpointIdHash)
    {
        return false;
    }

    {
        const auto audioSsrcRewriteMapItr = _audioSsrcRewriteMap.find(endpointIdHash);
        if (audioSsrcRewriteMapItr != _audioSsrcRewriteMap.end())
        {
            if (!_activeAudioList.remove(endpointIdHash))
            {
                assert(false);
                return false;
            }
            const auto pushResult = _activeAudioList.pushToTail(endpointIdHash);
            assert(pushResult);
            return true;
        }
    }

    if (_audioSsrcRewriteMap.size() == _maxActiveListSize)
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
        const auto rewriteMapItr = _videoSsrcRewriteMap.find(removedEndpointIdHash);
        if (rewriteMapItr != _videoSsrcRewriteMap.end())
        {
            const auto simulcastLevel = rewriteMapItr->second;
            _videoSsrcRewriteMap.erase(removedEndpointIdHash);
            _reverseVideoSsrcRewriteMap.erase(simulcastLevel._ssrc);
            _videoSsrcs.push(simulcastLevel);
        }
    }

    if (videoParticipant._simulcastStream.isSendingVideo() ||
        (videoParticipant._secondarySimulcastStream.isSet() &&
            videoParticipant._secondarySimulcastStream.get().isSendingVideo()))
    {
        SimulcastLevel simulcastLevel;
        if (!_videoSsrcs.pop(simulcastLevel))
        {
            assert(false);
            return false;
        }

        _videoSsrcRewriteMap.emplace(endpointIdHash, simulcastLevel);
        _reverseVideoSsrcRewriteMap.emplace(simulcastLevel._ssrc, endpointIdHash);
    }

    const bool addResult = _activeVideoList.pushToTail(endpointIdHash);
    assert(addResult);
    _activeVideoListLookupMap.emplace(endpointIdHash, _activeVideoList.tail());
    return true;
}

bool ActiveMediaList::makeLastNListMessage(const size_t lastN,
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

    api::DataChannelMessage::makeLastNStart(outMessage);
    bool isFirstElement = true;
    size_t i = 0;

    if (pinTargetEndpointIdHash)
    {
        const auto videoStreamItr = engineVideoStreams.find(pinTargetEndpointIdHash);
        if (videoStreamItr != engineVideoStreams.cend())
        {
            api::DataChannelMessage::makeLastNAppend(outMessage, videoStreamItr->second->_endpointId, isFirstElement);
            isFirstElement = false;
            ++i;
        }
    }

    auto participantEntry = _activeVideoList.tail();
    while (participantEntry && i < lastN)
    {
        if (participantEntry->_data != pinTargetEndpointIdHash && participantEntry->_data != endpointIdHash)
        {
            const auto videoStreamItr = engineVideoStreams.find(participantEntry->_data);
            if (videoStreamItr != engineVideoStreams.cend())
            {
                api::DataChannelMessage::makeLastNAppend(outMessage,
                    videoStreamItr->second->_endpointId,
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
    const concurrency::MpmcHashmap32<size_t, EngineAudioStream*>& engineAudioStreams,
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
                api::DataChannelMessage::addUserMediaSsrc(outMessage, videoStream->_pinSsrc.get()._ssrc);
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

    auto videoListEntry = _activeVideoList.tail();
    while (videoListEntry && addedElements < lastN)
    {
        if (videoListEntry->_data == endpointIdHash ||
            (videoListEntry->_data == pinTargetEndpointIdHash && !isPinTargetInActiveVideoList))
        {
            videoListEntry = videoListEntry->_previous;
            continue;
        }

        const auto videoStreamItr = engineVideoStreams.find(videoListEntry->_data);
        if (videoStreamItr == engineVideoStreams.end())
        {
            videoListEntry = videoListEntry->_previous;
            continue;
        }
        const auto videoStream = videoStreamItr->second;

        api::DataChannelMessage::addUserMediaEndpointStart(outMessage, videoStream->_endpointId.c_str());

        const auto rewriteMapItr = _videoSsrcRewriteMap.find(videoListEntry->_data);
        if (rewriteMapItr != _videoSsrcRewriteMap.end())
        {
            api::DataChannelMessage::addUserMediaSsrc(outMessage, rewriteMapItr->second._ssrc);
        }

        if (_videoScreenShareSsrcMapping.isSet() && _videoScreenShareSsrcMapping.get().first == videoListEntry->_data)
        {
            api::DataChannelMessage::addUserMediaSsrc(outMessage,
                _videoScreenShareSsrcMapping.get().second._rewriteSsrc);
        }

        api::DataChannelMessage::addUserMediaEndpointEnd(outMessage);
        ++addedElements;
        videoListEntry = videoListEntry->_previous;
    }

    api::DataChannelMessage::addUserMediaMapEnd(outMessage);
    return true;
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
        assert(_videoSsrcRewriteMap.size() == _reverseVideoSsrcRewriteMap.size());

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
