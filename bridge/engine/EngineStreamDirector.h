#pragma once

#include "bridge/engine/SimulcastStream.h"
#include "bwe/BandwidthUtils.h"
#include "concurrency/MpmcHashmap.h"
#include "logger/Logger.h"
#include "utils/Optional.h"
#include "utils/Time.h"
#include <atomic>
#include <cstdint>

#define DEBUG_DIRECTOR 0

#if DEBUG_DIRECTOR
#define DIRECTOR_LOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
#define DIRECTOR_LOG(fmt, ...)
#endif

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
        /** Min of incoming estimate and EngineStreamDirector::_maxDefaultLevelBandwidthKbps */
        uint32_t _defaultLevelBandwidthLimit;
    };

    EngineStreamDirector()
        : _participantStreams(maxParticipants),
          _pinMap(maxParticipants),
          _reversePinMap(maxParticipants),
          _lowQualitySsrcs(maxParticipants),
          _midQualitySsrcs(maxParticipants),
          _bandwidthFloor(0),
          _requiredMidLevelBandwidth(0),
          _maxDefaultLevelBandwidthKbps(3000)
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

        _lowQualitySsrcs.emplace(primary._levels[lowQuality]._ssrc, endpointIdHash);
        if (primary._numLevels > 1)
        {
            _midQualitySsrcs.emplace(primary._levels[midQuality]._ssrc, endpointIdHash);
        }
        _requiredMidLevelBandwidth += bwe::BandwidthUtils::getSimulcastLevelKbps(midQuality);

        if (secondary)
        {
            _participantStreams.emplace(endpointIdHash,
                makeParticipantStreams(primary, utils::Optional<SimulcastStream>(*secondary)));
            _lowQualitySsrcs.emplace(secondary->_levels[lowQuality]._ssrc, endpointIdHash);
            if (secondary->_numLevels > 1)
            {
                _midQualitySsrcs.emplace(secondary->_levels[midQuality]._ssrc, endpointIdHash);
            }
            _requiredMidLevelBandwidth += bwe::BandwidthUtils::getSimulcastLevelKbps(midQuality);

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

        if (participantStream._primary._numLevels > 0)
        {
            _lowQualitySsrcs.erase(participantStream._primary._levels[lowQuality]._ssrc);
            _midQualitySsrcs.erase(participantStream._primary._levels[midQuality]._ssrc);
            assert(_requiredMidLevelBandwidth > 0);
            _requiredMidLevelBandwidth -= bwe::BandwidthUtils::getSimulcastLevelKbps(midQuality);
        }
        if (participantStream._secondary.isSet() && participantStream._secondary.get()._numLevels > 0)
        {
            _lowQualitySsrcs.erase(participantStream._secondary.get()._levels[lowQuality]._ssrc);
            _midQualitySsrcs.erase(participantStream._secondary.get()._levels[midQuality]._ssrc);
            assert(_requiredMidLevelBandwidth > 0);
            _requiredMidLevelBandwidth -= bwe::BandwidthUtils::getSimulcastLevelKbps(midQuality);
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
        _bandwidthFloor = bwe::BandwidthUtils::calcBandwidthFloor(lowQuality, lastN, audioStreams, videoStreams);
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
    bool setUplinkEstimateKbps(const size_t endpointIdHash, const uint32_t uplinkEstimateKbps, const uint64_t timestamp)
    {
        auto participantStreamsItr = _participantStreams.find(endpointIdHash);
        if (participantStreamsItr == _participantStreams.end())
        {
            return false;
        }
        auto& participantStream = participantStreamsItr->second;

        participantStream._defaultLevelBandwidthLimit = std::min(uplinkEstimateKbps, _maxDefaultLevelBandwidthKbps);
        participantStream._desiredHighestEstimatedPinnedLevel =
            bwe::BandwidthUtils::calcPinnedHighestSimulcastLevel(lowQuality, _bandwidthFloor, uplinkEstimateKbps);

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

    /**
     * This function is the filter used for incoming video packets.
     * @return true if a participant is likely to be interested in the ssrc (do not drop packet) or false if
     * no participant is interested in the ssrc (packet should be dropped).
     */
    inline bool isSsrcUsed(const uint32_t ssrc,
        const size_t senderEndpointIdHash,
        const bool isSenderInLastNList,
        const size_t numRecordingStreams)
    {
        if (isUnpinnedQualityUsed(ssrc, senderEndpointIdHash, isSenderInLastNList))
        {
            DIRECTOR_LOG("isSsrcUsed, %u default", "EngineStreamDirector", ssrc);
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
                DIRECTOR_LOG("isSsrcUsed, %u pinned high", "EngineStreamDirector", ssrc);
                return true;
            }
        }

        if (numRecordingStreams != 0)
        {
            const auto result = isContentSlides(ssrc, senderEndpointIdHash);
            DIRECTOR_LOG("isSsrcUsed isContentSlides %u: result %c", "EngineStreamDirector", ssrc, result ? 't' : 'f');
            return result;
        }

        DIRECTOR_LOG("isSsrcUsed, %u false", "EngineStreamDirector", ssrc);
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
                DIRECTOR_LOG("shouldForwardSsrc toEndpointIdHash %lu ssrc %u: %c, pin target high quality",
                    "EngineStreamDirector",
                    toEndpointIdHash,
                    ssrc,
                    result ? 't' : 'f');
                return result;
            }

            const auto lowQualitySsrcsItr = _lowQualitySsrcs.find(ssrc);
            const auto result =
                lowQualitySsrcsItr != _lowQualitySsrcs.end() && lowQualitySsrcsItr->second != toEndpointIdHash;

            DIRECTOR_LOG("shouldForwardSsrc toEndpointIdHash %lu ssrc %u: %c, non pin target low quality",
                "EngineStreamDirector",
                toEndpointIdHash,
                ssrc,
                result ? 't' : 'f');

            return result;
        }

        const auto lowQualitySsrcsItr = _lowQualitySsrcs.find(ssrc);
        const auto midQualitySsrcsItr = _midQualitySsrcs.find(ssrc);
        if (lowQualitySsrcsItr == _lowQualitySsrcs.end() && midQualitySsrcsItr == _midQualitySsrcs.end())
        {
            DIRECTOR_LOG("shouldForwardSsrc toEndpointIdHash %lu ssrc %u: result f, in low f, in mid f, unpinned",
                "EngineStreamDirector",
                toEndpointIdHash,
                ssrc);
            return false;
        }

        bool result = false;

        const auto viewedByParticipantStreamItr = _participantStreams.find(toEndpointIdHash);
        if (viewedByParticipantStreamItr == _participantStreams.end())
        {
            result = false;
        }
        const auto wantedDefaultLevelQuality = getWantedDefaultLevelQuality(viewedByParticipantStreamItr->second);

        if (wantedDefaultLevelQuality == lowQuality)
        {
            result = lowQualitySsrcsItr != _lowQualitySsrcs.end() && lowQualitySsrcsItr->second != toEndpointIdHash;
        }
        else if (wantedDefaultLevelQuality == midQuality)
        {
            if (lowQualitySsrcsItr != _lowQualitySsrcs.end())
            {
                const auto highestActiveQuality = getParticipantHighestActiveQuality(lowQualitySsrcsItr->second, ssrc);
                result = highestActiveQuality == lowQuality && lowQualitySsrcsItr->second != toEndpointIdHash;
            }
            else if (midQualitySsrcsItr != _midQualitySsrcs.end())
            {
                result = midQualitySsrcsItr->second != toEndpointIdHash;
            }
        }

        DIRECTOR_LOG(
            "shouldForwardSsrc toEndpointIdHash %lu ssrc %u: result %c, in low %c, in mid %c, "
            "wantedDefaultLevelQuality %lu, requiredMidLevelBandwidth %u, _defaultLevelBandwidthLimit %u, unpinned",
            "EngineStreamDirector",
            toEndpointIdHash,
            ssrc,
            result ? 't' : 'f',
            lowQualitySsrcsItr != _lowQualitySsrcs.end() ? 't' : 'f',
            midQualitySsrcsItr != _midQualitySsrcs.end() ? 't' : 'f',
            wantedDefaultLevelQuality,
            _requiredMidLevelBandwidth,
            viewedByParticipantStreamItr->second._defaultLevelBandwidthLimit);

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
        const auto usedDefaultSsrcsItr = _lowQualitySsrcs.find(ssrc);
        if (usedDefaultSsrcsItr == _lowQualitySsrcs.end())
        {
            return 0;
        }

        return usedDefaultSsrcsItr->second;
    }

    inline bool getFeedbackSsrc(const uint32_t defaultLevelSsrc, uint32_t& outDefaultLevelFeedbackSsrc)
    {
        auto usedDefaultSsrcItr = _lowQualitySsrcs.find(defaultLevelSsrc);
        if (usedDefaultSsrcItr == _lowQualitySsrcs.end())
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
        if (defaultLevelSsrc == primary._levels[lowQuality]._ssrc)
        {
            simulcastStream = &primary;
        }
        else if (secondary.isSet() && defaultLevelSsrc == secondary.get()._levels[lowQuality]._ssrc)
        {
            simulcastStream = &secondary.get();
        }
        else
        {
            assert(false);
            return false;
        }

        outDefaultLevelFeedbackSsrc = simulcastStream->_levels[lowQuality]._feedbackSsrc;
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
    static constexpr size_t lowQuality = 0;
    static constexpr size_t midQuality = 1;

    /** Important: This has to be a lot bigger than the actual maximum participants per conference since we have
     * to avoid map entry reuse. Currently multiplied by 2 for that reason. */
    static constexpr size_t maxParticipants = 1024 * 2;
    static const uint64_t timeBeforeScaleUpMs = 5000ULL;

    concurrency::MpmcHashmap32<size_t, ParticipantStreams> _participantStreams;
    concurrency::MpmcHashmap32<size_t, size_t> _pinMap;
    concurrency::MpmcHashmap32<size_t, size_t> _reversePinMap;
    concurrency::MpmcHashmap32<uint32_t, size_t> _lowQualitySsrcs;
    concurrency::MpmcHashmap32<uint32_t, size_t> _midQualitySsrcs;
    uint32_t _bandwidthFloor;

    /** Bandwidth required to send the mid level as default level for participants without pin targets */
    uint32_t _requiredMidLevelBandwidth;

    /** Bandwidth cap for sending default levels to participants without pin targets */
    uint32_t _maxDefaultLevelBandwidthKbps;

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

    inline size_t getParticipantHighestActiveQuality(const size_t endpointIdHash, const uint32_t ssrc)
    {
        const auto participantStreamsItr = _participantStreams.find(endpointIdHash);
        if (participantStreamsItr == _participantStreams.end())
        {
            return 0;
        }

        auto& participantStreams = participantStreamsItr->second;
        const auto& primary = participantStreams._primary;
        const auto& secondaryOptional = participantStreams._secondary;

        if (ssrc == primary._levels[0]._ssrc || ssrc == primary._levels[1]._ssrc || ssrc == primary._levels[2]._ssrc)
        {
            return primary._highestActiveLevel;
        }

        if (secondaryOptional.isSet())
        {
            const auto& secondary = secondaryOptional.get();
            if (ssrc == secondary._levels[0]._ssrc || ssrc == secondary._levels[1]._ssrc ||
                ssrc == secondary._levels[2]._ssrc)
            {
                return secondary._highestActiveLevel;
            }
        }

        assert(false);
        return 0;
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
            0,
            _maxDefaultLevelBandwidthKbps};
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

    inline bool anyParticipantsWithoutPinTarget() const { return _participantStreams.size() != _pinMap.size(); }

    /**
     * Checks for ssrcs belonging to a default level, either low quality when there are no participants
     * without pin targets, or low and mid quality if there are participants without pin targets.
     */
    inline bool isUnpinnedQualityUsed(const uint32_t ssrc,
        const size_t senderEndpointIdHash,
        const bool isSenderInLastNList)
    {
        if (!isSenderInLastNList)
        {
            return false;
        }

        if (anyParticipantsWithoutPinTarget())
        {
            return _lowQualitySsrcs.contains(ssrc) || _midQualitySsrcs.contains(ssrc);
        }
        else
        {
            return _lowQualitySsrcs.contains(ssrc) && !isPinnedByAll(senderEndpointIdHash);
        }
    }

    inline size_t getWantedDefaultLevelQuality(const ParticipantStreams& participantStreams) const
    {
        return _requiredMidLevelBandwidth > participantStreams._defaultLevelBandwidthLimit ? lowQuality : midQuality;
    }
};

} // namespace bridge

