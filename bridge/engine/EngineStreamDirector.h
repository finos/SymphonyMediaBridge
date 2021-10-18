#pragma once

#include "bridge/engine/SimulcastStream.h"
#include "bwe/BandwidthUtils.h"
#include "concurrency/MpmcHashmap.h"
#include "logger/Logger.h"
#include "utils/Optional.h"
#include "utils/Time.h"
#include <cstdint>

#define DEBUG_DIRECTOR 0

namespace bridge
{

struct EngineAudioStream;

class EngineStreamDirector
{
public:
    struct ParticipantStreams
    {
        SimulcastStream _primary;
        utils::Optional<SimulcastStream> _secondary;
        size_t _highestEstimatedPinnedLevel;
        size_t _desiredHighestEstimatedPinnedLevel;
        uint64_t _lowEstimateTimestamp;
    };

    EngineStreamDirector()
        : _participantStreams(maxParticipants),
          _pinMap(maxParticipants),
          _reversePinMap(maxParticipants),
          _usedDefaultSsrcs(maxParticipants),
          _bandwidthFloor(0)
    {
    }

    void addParticipant(const size_t endpointIdHash)
    {
        if (_participantStreams.find(endpointIdHash) != _participantStreams.end())
        {
            logger::debug("addParticipant stream already added, endpointIdHash %lu",
                "EngineStreamDirector",
                endpointIdHash);
            return;
        }

        logger::debug("addParticipant, endpointIdHash %lu", "EngineStreamDirector", endpointIdHash);

        SimulcastStream emptyStream;
        memset(&emptyStream, 0, sizeof(SimulcastStream));
        _participantStreams.emplace(endpointIdHash,
            makeParticipantStreams(emptyStream, utils::Optional<SimulcastStream>()));
    }

    void addParticipant(const size_t endpointIdHash,
        const SimulcastStream& primary,
        const SimulcastStream* secondary = nullptr)
    {
        if (_participantStreams.find(endpointIdHash) != _participantStreams.end())
        {
            logger::debug("addParticipant stream already added, endpointIdHash %lu",
                "EngineStreamDirector",
                endpointIdHash);
            return;
        }

        logger::info("addParticipant primary, endpointIdHash %lu, %u %u %u",
            "EngineStreamDirector",
            endpointIdHash,
            primary._levels[0]._ssrc,
            primary._levels[1]._ssrc,
            primary._levels[2]._ssrc);

        _usedDefaultSsrcs.emplace(primary._levels[defaultQuality]._ssrc, endpointIdHash);

        if (secondary)
        {
            _participantStreams.emplace(endpointIdHash,
                makeParticipantStreams(primary, utils::Optional<SimulcastStream>(*secondary)));
            _usedDefaultSsrcs.emplace(secondary->_levels[defaultQuality]._ssrc, endpointIdHash);

            logger::info("addParticipant secondary, endpointIdHash %lu, %u %u %u",
                "EngineStreamDirector",
                endpointIdHash,
                secondary->_levels[0]._ssrc,
                secondary->_levels[1]._ssrc,
                secondary->_levels[2]._ssrc);
        }
        else
        {
            _participantStreams.emplace(endpointIdHash,
                makeParticipantStreams(primary, utils::Optional<SimulcastStream>()));
        }
    }

    void removeParticipant(const size_t endpointIdHash)
    {
        auto participantStreamsItr = _participantStreams.find(endpointIdHash);
        if (participantStreamsItr == _participantStreams.end())
        {
            return;
        }
        auto& participantStream = participantStreamsItr->second;

        _usedDefaultSsrcs.erase(participantStream._primary._levels[defaultQuality]._ssrc);
        if (participantStream._secondary.isSet())
        {
            _usedDefaultSsrcs.erase(participantStream._secondary.get()._levels[defaultQuality]._ssrc);
        }
        _participantStreams.erase(endpointIdHash);

        logger::info("removeParticipant, endpointIdHash %lu", "EngineStreamDirector", endpointIdHash);
        return;
    }

    void removeParticipantPins(const size_t endpointIdHash)
    {
        const auto pinMapItr = _pinMap.find(endpointIdHash);
        if (pinMapItr != _pinMap.end() && pinMapItr->second != 0)
        {
            const auto pinTarget = pinMapItr->second;
            auto reversePinMapItr = _reversePinMap.find(pinTarget);
            if (reversePinMapItr != _reversePinMap.end())
            {
                auto count = reversePinMapItr->second;
                _reversePinMap.erase(pinTarget);
                if (count > 0)
                {
                    --count;
                    _reversePinMap.emplace(pinTarget, count);
                }
            }
        }

        _pinMap.erase(endpointIdHash);
        _reversePinMap.erase(endpointIdHash);

        for (const auto& pinMapEntry : _pinMap)
        {
            if (pinMapEntry.second == endpointIdHash)
            {
                logger::debug("removeSimulcastStream, removed reverse pin endpointIdHash %lu target %lu",
                    "EngineStreamDirector",
                    pinMapEntry.first,
                    endpointIdHash);
                _pinMap.erase(pinMapEntry.second);
            }
        }
    }

    size_t pin(const size_t endpointIdHash, const size_t targetEndpointIdHash)
    {
        const auto oldTarget = unpinOldTarget(endpointIdHash, targetEndpointIdHash);
        if (oldTarget == targetEndpointIdHash)
        {
            return oldTarget;
        }

        if (targetEndpointIdHash)
        {
            size_t count = 0;
            auto reversePinMapItr = _reversePinMap.find(targetEndpointIdHash);
            if (reversePinMapItr != _reversePinMap.end())
            {
                count = reversePinMapItr->second;
                _reversePinMap.erase(targetEndpointIdHash);
            }
            ++count;
            _reversePinMap.emplace(targetEndpointIdHash, count);
            _pinMap.emplace(endpointIdHash, targetEndpointIdHash);
        }

        logger::info("pin, endpointIdHash %lu, targetEndpointIdHash %lu, oldTarget %lu",
            "EngineStreamDirector",
            endpointIdHash,
            targetEndpointIdHash,
            oldTarget);

        return oldTarget;
    }

    size_t getPinTarget(const size_t endpointIdHash)
    {
        auto pinMapItr = _pinMap.find(endpointIdHash);
        if (pinMapItr == _pinMap.end())
        {
            return 0;
        }

        return pinMapItr->second;
    }

    void updateBandwidthFloor(const uint32_t lastN, const uint32_t audioStreams, const uint32_t videoStreams)
    {
        _bandwidthFloor = bwe::BandwidthUtils::calcBandwidthFloor(defaultQuality, lastN, audioStreams, videoStreams);
        logger::debug("updateBandwidthFloor lastN %u, audioStreams %u, videoStreams %u -> %u",
            "EngineStreamDirector",
            lastN,
            audioStreams,
            videoStreams,
            _bandwidthFloor);
    }

    /**
     * @return true if the highest estimated level changed. (User could get a higher or lower simulcast level on
     * the pinned stream.
     */
    bool setUplinkEstimateKbps(const size_t endpointIdHash,
        const uint32_t uplinkEstimateKbps,
        const uint64_t timestamp,
        const bool isPadding)
    {
        auto participantStreamsItr = _participantStreams.find(endpointIdHash);
        if (participantStreamsItr == _participantStreams.end())
        {
            return false;
        }
        auto& participantStream = participantStreamsItr->second;

        if (isPadding)
        {
            participantStream._desiredHighestEstimatedPinnedLevel = 0;
            participantStream._highestEstimatedPinnedLevel = 0;

            logger::info("setUplinkEstimateKbps %u, endpointIdHash %lu, ignoring estimates sending padding",
                "EngineStreamDirector",
                uplinkEstimateKbps,
                endpointIdHash);
            return false;
        }

        participantStream._desiredHighestEstimatedPinnedLevel =
            bwe::BandwidthUtils::calcPinnedHighestSimulcastLevel(defaultQuality, _bandwidthFloor, uplinkEstimateKbps);

        if (participantStream._desiredHighestEstimatedPinnedLevel == participantStream._highestEstimatedPinnedLevel)
        {
            participantStream._lowEstimateTimestamp = timestamp;
            return false;
        }
        else if (participantStream._desiredHighestEstimatedPinnedLevel < participantStream._highestEstimatedPinnedLevel)
        {
            logger::info("setUplinkEstimateKbps %u, endpointIdHash %lu, desiredLevel %lu < level %lu, scale down",
                "EngineStreamDirector",
                uplinkEstimateKbps,
                endpointIdHash,
                participantStream._desiredHighestEstimatedPinnedLevel,
                participantStream._highestEstimatedPinnedLevel);

            participantStream._highestEstimatedPinnedLevel = participantStream._desiredHighestEstimatedPinnedLevel;
            participantStream._lowEstimateTimestamp = timestamp;
            return true;
        }

        logger::debug("setUplinkEstimateKbps %u, endpointIdHash %lu desiredLevel %lu > level %lu",
            "EngineStreamDirector",
            uplinkEstimateKbps,
            endpointIdHash,
            participantStream._desiredHighestEstimatedPinnedLevel,
            participantStream._highestEstimatedPinnedLevel);

        if (utils::Time::diffGE(participantStream._lowEstimateTimestamp,
                timestamp,
                timeBeforeScaleUpMs * utils::Time::ms))
        {
            logger::info("setUplinkEstimateKbps %u, endpointIdHash %lu desiredLevel %lu > level %lu, scale up",
                "EngineStreamDirector",
                uplinkEstimateKbps,
                endpointIdHash,
                participantStream._desiredHighestEstimatedPinnedLevel,
                participantStream._highestEstimatedPinnedLevel);

            participantStream._highestEstimatedPinnedLevel = participantStream._desiredHighestEstimatedPinnedLevel;
            participantStream._lowEstimateTimestamp = timestamp;
            return true;
        }

        return false;
    }

    inline uint32_t getTargetBitrateKbps(const uint32_t lastN)
    {
        return bwe::BandwidthUtils::calcBandwidthFloor(defaultQuality, lastN, lastN, lastN) +
            bwe::BandwidthUtils::getSimulcastLevelKbps(SimulcastStream::maxLevels - 1);
    }

    inline bool isSsrcUsed(const uint32_t ssrc,
        const size_t senderEndpointIdHash,
        const bool isSenderInLastNList,
        const size_t numRecordingStreams)
    {
        if (_usedDefaultSsrcs.find(ssrc) != _usedDefaultSsrcs.end() && isSenderInLastNList &&
            !isPinnedByAll(senderEndpointIdHash))
        {
#if DEBUG_DIRECTOR
            logger::debug("isSsrcUsed, %u default", "EngineStreamDirector", ssrc);
#endif
            return true;
        }

        const auto reversePinMapItr = _reversePinMap.find(senderEndpointIdHash);
        if (reversePinMapItr != _reversePinMap.end() && reversePinMapItr->second == 0)
        {
            return false;
        }

        for (const auto& pinMapEntry : _pinMap)
        {
            if (isParticipantHighestActiveQuality(pinMapEntry.second, pinMapEntry.first, ssrc))
            {
#if DEBUG_DIRECTOR
                logger::debug("isSsrcUsed, %u pinned high", "EngineStreamDirector", ssrc);
#endif
                return true;
            }
        }

        if (numRecordingStreams != 0)
        {
            const auto result = isContentSlides(ssrc, senderEndpointIdHash);
#if DEBUG_DIRECTOR
            logger::debug("isSsrcUsed isContentSlides %u: result %c", "EngineStreamDirector", ssrc, result ? 't' : 'f');
#endif
            return result;
        }

#if DEBUG_DIRECTOR
        logger::debug("isSsrcUsed, %u false", "EngineStreamDirector", ssrc);
#endif
        return false;
    }

    inline bool shouldForwardSsrc(const size_t toEndpointIdHash, const uint32_t ssrc)
    {
        const auto pinMapItr = _pinMap.find(toEndpointIdHash);
        if (pinMapItr != _pinMap.end())
        {
            if (isSsrcFromParticipant(pinMapItr->second, ssrc))
            {
                const auto result = isParticipantHighestActiveQuality(pinMapItr->second, toEndpointIdHash, ssrc);
#if DEBUG_DIRECTOR
                logger::debug("shouldForwardSsrc toEndpointIdHash %lu ssrc %u: %c, dominant high quality",
                    "EngineStreamDirector",
                    toEndpointIdHash,
                    ssrc,
                    result ? 't' : 'f');
#endif
                return result;
            }
        }

        const auto defaultSsrcItr = _usedDefaultSsrcs.find(ssrc);
        const auto result = defaultSsrcItr != _usedDefaultSsrcs.end() && defaultSsrcItr->second != toEndpointIdHash;
#if DEBUG_DIRECTOR
        logger::debug(
            "shouldForwardSsrc toEndpointIdHash %lu ssrc %u: result %c, in default %c, toEndpointIdHash default",
            "EngineStreamDirector",
            toEndpointIdHash,
            ssrc,
            result ? 't' : 'f',
            defaultSsrcItr != _usedDefaultSsrcs.end() ? 't' : 'f');
#endif
        return result;
    }

    /**
     * This is called in parallel with add/remove. This is ok as long as the _participantStreams map has a lot of spare
     * space. Since this function will possibly access elements after removal. MpmcMap does not return memory for
     * removed elements to the OS and it does not reuse removed elements until all other free elements are reused.
     */
    bool streamActiveStateChanged(const size_t endpointIdHash, const uint32_t ssrc, const bool active)
    {
        auto participantStreamsItr = _participantStreams.find(endpointIdHash);
        if (participantStreamsItr == _participantStreams.end())
        {
            return false;
        }
        auto& participantStreams = participantStreamsItr->second;
        auto& primary = participantStreams._primary;
        auto& secondary = participantStreams._secondary;

        logger::info("streamActiveStateChanged, endpointIdHash %lu, ssrc %u, active %c",
            "EngineStreamDirector",
            endpointIdHash,
            ssrc,
            active ? 't' : 'f');

        for (size_t i = 0; i < primary._numLevels; ++i)
        {
            if (ssrc == primary._levels[i]._ssrc)
            {
                primary._levels[i]._mediaActive = active;
                return setHighestActiveIndex(endpointIdHash, primary);
            }
        }

        if (secondary.isSet())
        {
            for (size_t i = 0; i < secondary.get()._numLevels; ++i)
            {
                if (ssrc == secondary.get()._levels[i]._ssrc)
                {
                    secondary.get()._levels[i]._mediaActive = active;
                    return setHighestActiveIndex(endpointIdHash, secondary.get());
                }
            }
        }

        return false;
    }

    inline size_t getParticipantForDefaultLevelSsrc(const uint32_t ssrc)
    {
        const auto usedDefaultSsrcsItr = _usedDefaultSsrcs.find(ssrc);
        if (usedDefaultSsrcsItr == _usedDefaultSsrcs.end())
        {
            return 0;
        }

        return usedDefaultSsrcsItr->second;
    }

    inline bool getFeedbackSsrc(const uint32_t defaultLevelSsrc, uint32_t& outDefaultLevelFeedbackSsrc)
    {
        auto usedDefaultSsrcItr = _usedDefaultSsrcs.find(defaultLevelSsrc);
        if (usedDefaultSsrcItr == _usedDefaultSsrcs.end())
        {
            return false;
        }

        auto sendingParticipant = usedDefaultSsrcItr->second;
        auto participantStreamsItr = _participantStreams.find(sendingParticipant);
        if (participantStreamsItr == _participantStreams.end())
        {
            return false;
        }
        auto& participantStreams = participantStreamsItr->second;
        auto& primary = participantStreams._primary;
        auto& secondary = participantStreams._secondary;

        SimulcastStream* simulcastStream;
        if (defaultLevelSsrc == primary._levels[defaultQuality]._ssrc)
        {
            simulcastStream = &primary;
        }
        else if (secondary.isSet() && defaultLevelSsrc == secondary.get()._levels[defaultQuality]._ssrc)
        {
            simulcastStream = &secondary.get();
        }
        else
        {
            assert(false);
            return false;
        }

        outDefaultLevelFeedbackSsrc = simulcastStream->_levels[defaultQuality]._feedbackSsrc;
        return true;
    }

    inline bool getSsrc(const size_t sendingEndpointIdHash, const uint32_t feedbackSsrc, uint32_t& outSsrc)
    {
        const auto participantStreamsItr = _participantStreams.find(sendingEndpointIdHash);
        if (participantStreamsItr == _participantStreams.end())
        {
            return false;
        }
        auto& participantStreams = participantStreamsItr->second;
        const auto& primary = participantStreams._primary;
        auto& secondary = participantStreams._secondary;

        for (size_t i = 0; i < primary._numLevels; ++i)
        {
            if (primary._levels[i]._feedbackSsrc == feedbackSsrc)
            {
                outSsrc = primary._levels[i]._ssrc;
                return true;
            }
        }

        if (secondary.isSet())
        {
            for (size_t i = 0; i < secondary.get()._numLevels; ++i)
            {
                if (secondary.get()._levels[i]._feedbackSsrc == feedbackSsrc)
                {
                    outSsrc = secondary.get()._levels[i]._ssrc;
                    return true;
                }
            }
        }

        return false;
    }

private:
    static const size_t defaultQuality = 0;

    /** Important: This has to be a lot bigger than the actual maximum participants per conference since we have
     * to avoid map entry reuse. Currently multiplied by 2 for that reason. */
    static constexpr size_t maxParticipants = 1024 * 2;
    static const uint64_t timeBeforeScaleUpMs = 5000ULL;

    concurrency::MpmcHashmap32<size_t, ParticipantStreams> _participantStreams;
    concurrency::MpmcHashmap32<size_t, size_t> _pinMap;
    concurrency::MpmcHashmap32<size_t, size_t> _reversePinMap;
    concurrency::MpmcHashmap32<uint32_t, size_t> _usedDefaultSsrcs;
    uint32_t _bandwidthFloor;

    inline bool isParticipantHighestActiveQuality(const size_t endpointIdHash,
        const size_t viewedByEndpointIdHash,
        const uint32_t ssrc)
    {
        const auto participantStreamsItr = _participantStreams.find(endpointIdHash);
        const auto viewedByParticipantStreamsItr = _participantStreams.find(viewedByEndpointIdHash);
        if (participantStreamsItr == _participantStreams.end() ||
            viewedByParticipantStreamsItr == _participantStreams.end())
        {
            return false;
        }

        auto& participantStreams = participantStreamsItr->second;
        const auto& primary = participantStreams._primary;
        auto& secondary = participantStreams._secondary;
        const auto& viewedByParticipantStreams = viewedByParticipantStreamsItr->second;

        const auto primaryDesiredLevel =
            std::min(viewedByParticipantStreams._highestEstimatedPinnedLevel, primary._highestActiveLevel);
        if (primary._numLevels > 0 && ssrc == primary._levels[primaryDesiredLevel]._ssrc)
        {
            return true;
        }

        if (secondary.isSet())
        {
            const auto secondaryDesiredLevel =
                std::min(viewedByParticipantStreams._highestEstimatedPinnedLevel, secondary.get()._highestActiveLevel);
            if (ssrc == secondary.get()._levels[secondaryDesiredLevel]._ssrc)
            {
                return true;
            }
        }

        return false;
    }

    inline bool isSsrcFromParticipant(const size_t endpointIdHash, const uint32_t ssrc)
    {
        const auto participantStreamsItr = _participantStreams.find(endpointIdHash);
        if (participantStreamsItr == _participantStreams.end())
        {
            return false;
        }
        auto& participantStreams = participantStreamsItr->second;
        const auto& primary = participantStreams._primary;
        auto& secondary = participantStreams._secondary;

        for (size_t i = 0; i < primary._numLevels; ++i)
        {
            if (ssrc == primary._levels[i]._ssrc)
            {
                return true;
            }
        }

        if (secondary.isSet())
        {
            for (size_t i = 0; i < secondary.get()._numLevels; ++i)
            {
                if (ssrc == secondary.get()._levels[i]._ssrc)
                {
                    return true;
                }
            }
        }

        return false;
    }

    inline bool setHighestActiveIndex(const size_t endpointIdHash, SimulcastStream& simulcastStream)
    {
        const auto oldHighestActiveIndex = simulcastStream._highestActiveLevel;

        simulcastStream._highestActiveLevel = 0;
        for (auto i = (SimulcastStream::maxLevels - 1); i > 0; --i)
        {
            if (simulcastStream._levels[i]._mediaActive)
            {
                simulcastStream._highestActiveLevel = i;
                break;
            }
        }

        if (simulcastStream._highestActiveLevel > oldHighestActiveIndex &&
            _reversePinMap.find(endpointIdHash) != _reversePinMap.end())
        {
            return true;
        }

        return oldHighestActiveIndex != simulcastStream._highestActiveLevel;
    }

    inline bool isPinnedByAll(const size_t senderEndpointIdHash)
    {
        const auto reversePinMapItr = _reversePinMap.find(senderEndpointIdHash);
        const auto numParticipants = _participantStreams.size();

        if (reversePinMapItr == _reversePinMap.end() || numParticipants == 0)
        {
            return false;
        }

        const auto pinnedCount = reversePinMapItr->second;
        return pinnedCount == numParticipants - 1;
    }

    inline size_t unpinOldTarget(const size_t endpointIdHash, const size_t targetEndpointIdHash)
    {
        auto pinMapItr = _pinMap.find(endpointIdHash);
        if (pinMapItr == _pinMap.end())
        {
            return 0;
        }

        const auto oldTarget = pinMapItr->second;
        if (oldTarget == targetEndpointIdHash)
        {
            return oldTarget;
        }
        _pinMap.erase(endpointIdHash);

        if (oldTarget == 0)
        {
            return 0;
        }

        auto reversePinMapItr = _reversePinMap.find(oldTarget);
        if (reversePinMapItr != _reversePinMap.end())
        {
            auto count = reversePinMapItr->second;
            _reversePinMap.erase(oldTarget);
            if (count > 0)
            {
                --count;
                _reversePinMap.emplace(oldTarget, count);
            }
        }

        return oldTarget;
    }

    inline ParticipantStreams makeParticipantStreams(const SimulcastStream& primary,
        const utils::Optional<SimulcastStream>& secondary)
    {
        return ParticipantStreams{primary,
            secondary,
            SimulcastStream::maxLevels - 1,
            SimulcastStream::maxLevels - 1,
            0};
    }

    inline bool isContentSlides(const uint32_t ssrc, const size_t senderEndpointIdHash)
    {
        const auto participantStreamsItr = _participantStreams.find(senderEndpointIdHash);
        if (participantStreamsItr == _participantStreams.end())
        {
            return false;
        }

        const auto& primary = participantStreamsItr->second._primary;

        if (primary._contentType == SimulcastStream::VideoContentType::SLIDES && primary._numLevels == 1 &&
            primary._levels[0]._ssrc == ssrc)
        {
            return true;
        }

        if (!participantStreamsItr->second._secondary.isSet())
        {
            return false;
        }
        const auto& secondary = participantStreamsItr->second._secondary.get();

        if (secondary._contentType == SimulcastStream::VideoContentType::SLIDES && secondary._numLevels == 1 &&
            secondary._levels[0]._ssrc == ssrc)
        {
            return true;
        }

        return false;
    }
};

} // namespace bridge
