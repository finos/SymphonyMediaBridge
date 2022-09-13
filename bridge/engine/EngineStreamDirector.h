#pragma once

#include "bridge/engine/SimulcastStream.h"
#include "bwe/BandwidthUtils.h"
#include "concurrency/MpmcHashmap.h"
#include "config/Config.h"
#include "logger/Logger.h"
#include "utils/Optional.h"
#include "utils/Time.h"
#include <cstdint>

#define DEBUG_DIRECTOR 0

#if DEBUG_DIRECTOR
#define DIRECTOR_LOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
#define DIRECTOR_LOG(fmt, ...)
#endif

namespace bridge
{

class EngineStreamDirector
{
public:
    struct ParticipantStreams
    {
        ParticipantStreams(const SimulcastStream& primary,
            const utils::Optional<SimulcastStream>& secondary,
            const uint32_t maxDefaultLevelBandwidthKbps)
            : primary(primary),
              secondary(secondary),
              highestEstimatedPinnedLevel(SimulcastStream::maxLevels - 1),
              desiredHighestEstimatedPinnedLevel(SimulcastStream::maxLevels - 1),
              desiredUnpinnedLevel(0),
              lowEstimateTimestamp(0),
              defaultLevelBandwidthLimit(maxDefaultLevelBandwidthKbps),
              estimatedUplinkBandwidth(0)
        {
        }
        SimulcastStream primary;
        utils::Optional<SimulcastStream> secondary;
        size_t highestEstimatedPinnedLevel;
        size_t desiredHighestEstimatedPinnedLevel;
        size_t desiredUnpinnedLevel;
        uint64_t lowEstimateTimestamp;
        /** Min of incoming estimate and EngineStreamDirector::maxDefaultLevelBandwidthKbps */
        uint32_t defaultLevelBandwidthLimit;
        /** Max of incoming estimate and defaultLevelBandwidthLimit */
        uint32_t estimatedUplinkBandwidth;
    };

    EngineStreamDirector(size_t logInstanceId, const config::Config& config, uint32_t lastN)
        : _loggableId("StreamDirector", logInstanceId),
          _participantStreams(maxParticipants),
          _pinMap(maxParticipants),
          _reversePinMap(maxParticipants),
          _lowQualitySsrcs(maxParticipants),
          _midQualitySsrcs(maxParticipants),
          _bandwidthFloor(0),
          _requiredMidLevelBandwidth(0),
          _maxDefaultLevelBandwidthKbps(config.maxDefaultLevelBandwidthKbps),
          _lastN(lastN),
          _slidesBitrateKbps(0)
    {
    }

    void addParticipant(const size_t endpointIdHash)
    {
        if (_participantStreams.find(endpointIdHash) != _participantStreams.end())
        {
            logger::debug("addParticipant stream already added, endpointIdHash %lu",
                _loggableId.c_str(),
                endpointIdHash);
            return;
        }

        logger::debug("addParticipant, endpointIdHash %lu", _loggableId.c_str(), endpointIdHash);

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
                _loggableId.c_str(),
                endpointIdHash);
            return;
        }

        logger::info("addParticipant primary, endpointIdHash %lu, %u %u %u",
            _loggableId.c_str(),
            endpointIdHash,
            primary.levels[0].ssrc,
            primary.levels[1].ssrc,
            primary.levels[2].ssrc);

        _lowQualitySsrcs.emplace(primary.levels[lowQuality].ssrc, endpointIdHash);
        if (primary.numLevels > 1)
        {
            _midQualitySsrcs.emplace(primary.levels[midQuality].ssrc, endpointIdHash);
        }
        _requiredMidLevelBandwidth += bwe::BandwidthUtils::getSimulcastLevelKbps(midQuality);

        if (secondary)
        {
            _participantStreams.emplace(endpointIdHash,
                makeParticipantStreams(primary, utils::Optional<SimulcastStream>(*secondary)));
            _lowQualitySsrcs.emplace(secondary->levels[lowQuality].ssrc, endpointIdHash);
            if (secondary->numLevels > 1)
            {
                _midQualitySsrcs.emplace(secondary->levels[midQuality].ssrc, endpointIdHash);
            }
            _requiredMidLevelBandwidth += bwe::BandwidthUtils::getSimulcastLevelKbps(midQuality);

            logger::info("addParticipant secondary, endpointIdHash %lu, %u %u %u",
                _loggableId.c_str(),
                endpointIdHash,
                secondary->levels[0].ssrc,
                secondary->levels[1].ssrc,
                secondary->levels[2].ssrc);
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

        if (participantStream.primary.numLevels > 0)
        {
            _lowQualitySsrcs.erase(participantStream.primary.levels[lowQuality].ssrc);
            _midQualitySsrcs.erase(participantStream.primary.levels[midQuality].ssrc);
            assert(_requiredMidLevelBandwidth > 0);
            _requiredMidLevelBandwidth -= bwe::BandwidthUtils::getSimulcastLevelKbps(midQuality);
        }
        if (participantStream.secondary.isSet() && participantStream.secondary.get().numLevels > 0)
        {
            _lowQualitySsrcs.erase(participantStream.secondary.get().levels[lowQuality].ssrc);
            _midQualitySsrcs.erase(participantStream.secondary.get().levels[midQuality].ssrc);
            assert(_requiredMidLevelBandwidth > 0);
            _requiredMidLevelBandwidth -= bwe::BandwidthUtils::getSimulcastLevelKbps(midQuality);
        }
        _participantStreams.erase(endpointIdHash);

        logger::info("removeParticipant, endpointIdHash %lu", _loggableId.c_str(), endpointIdHash);
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
                    _loggableId.c_str(),
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
            _loggableId.c_str(),
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
            _loggableId.c_str(),
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

        participantStream.defaultLevelBandwidthLimit = std::min(uplinkEstimateKbps, _maxDefaultLevelBandwidthKbps);
        participantStream.estimatedUplinkBandwidth =
            std::max(uplinkEstimateKbps, participantStream.defaultLevelBandwidthLimit);

        size_t pinnedQuality, unpinnedQuality;
        getVideoQualityLimits(participantStream, pinnedQuality, unpinnedQuality);

        participantStream.desiredHighestEstimatedPinnedLevel = pinnedQuality;
        participantStream.desiredUnpinnedLevel = unpinnedQuality;

        if (participantStream.desiredHighestEstimatedPinnedLevel == participantStream.highestEstimatedPinnedLevel)
        {
            participantStream.lowEstimateTimestamp = timestamp;
            return false;
        }
        else if (participantStream.desiredHighestEstimatedPinnedLevel < participantStream.highestEstimatedPinnedLevel)
        {
            logger::info("setUplinkEstimateKbps %u, endpointIdHash %lu, desiredLevel %lu < level %lu, scale down",
                _loggableId.c_str(),
                uplinkEstimateKbps,
                endpointIdHash,
                participantStream.desiredHighestEstimatedPinnedLevel,
                participantStream.highestEstimatedPinnedLevel);

            participantStream.highestEstimatedPinnedLevel = participantStream.desiredHighestEstimatedPinnedLevel;
            participantStream.lowEstimateTimestamp = timestamp;
            return true;
        }

        logger::debug("setUplinkEstimateKbps %u, endpointIdHash %lu desiredLevel %lu > level %lu",
            _loggableId.c_str(),
            uplinkEstimateKbps,
            endpointIdHash,
            participantStream.desiredHighestEstimatedPinnedLevel,
            participantStream.highestEstimatedPinnedLevel);

        if (utils::Time::diffGE(participantStream.lowEstimateTimestamp,
                timestamp,
                timeBeforeScaleUpMs * utils::Time::ms))
        {
            logger::info("setUplinkEstimateKbps %u, endpointIdHash %lu desiredLevel %lu > level %lu, scale up",
                _loggableId.c_str(),
                uplinkEstimateKbps,
                endpointIdHash,
                participantStream.desiredHighestEstimatedPinnedLevel,
                participantStream.highestEstimatedPinnedLevel);

            participantStream.highestEstimatedPinnedLevel = participantStream.desiredHighestEstimatedPinnedLevel;
            participantStream.lowEstimateTimestamp = timestamp;
            return true;
        }

        return false;
    }

    inline size_t getQualityLevel(const uint32_t ssrc)
    {
        return (_lowQualitySsrcs.contains(ssrc)   ? lowQuality
                : _midQualitySsrcs.contains(ssrc) ? midQuality
                                                  : highQuality);
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
        if (_participantStreams.find(senderEndpointIdHash) == _participantStreams.end())
        {
            DIRECTOR_LOG("isSsrcUsed, %u false, endpoint removed", _loggableId.c_str(), ssrc);
            return false;
        }

        const auto quality = getQualityLevel(ssrc);
        const auto highestAvailableQuality = highestActiveQuality(senderEndpointIdHash, ssrc);
        if (highestAvailableQuality == dropQuality)
        {
            DIRECTOR_LOG("isSsrcUsed, %u false, ssrc not found", _loggableId.c_str(), ssrc);
            return false;
        }

        if (isUsedForUnpinnedVideo(quality, highestAvailableQuality, senderEndpointIdHash, isSenderInLastNList))
        {
            DIRECTOR_LOG("isSsrcUsed, %u default", _loggableId.c_str(), ssrc);
            return true;
        }

        if (isUsedForPinnedVideo(quality, highestAvailableQuality, senderEndpointIdHash))
        {
            DIRECTOR_LOG("isSsrcUsed, %u pinned %s",
                _loggableId.c_str(),
                ssrc,
                lowQuality == quality       ? "low"
                    : midQuality == quality ? "mid"
                                            : "high");
            return true;
        }

        if (isUsedForRecordingSlides(ssrc, senderEndpointIdHash, numRecordingStreams))
        {
            DIRECTOR_LOG("isSsrcUsed isContentSlides %u: result %c", _loggableId.c_str(), ssrc, result ? 't' : 'f');
            return true;
        }

        DIRECTOR_LOG("isSsrcUsed, %u false", _loggableId.c_str(), ssrc);
        return false;
    }

    inline size_t getCurrentQualityAndEndpointId(const uint32_t ssrc, size_t& outFromEndpointId)
    {
        const auto lowQualitySsrcsItr = _lowQualitySsrcs.find(ssrc);
        const auto midQualitySsrcsItr = _midQualitySsrcs.find(ssrc);
        if (lowQualitySsrcsItr != _lowQualitySsrcs.end())
        {
            outFromEndpointId = lowQualitySsrcsItr->second;
            return lowQuality;
        }
        if (midQualitySsrcsItr != _midQualitySsrcs.end())
        {
            outFromEndpointId = midQualitySsrcsItr->second;
            return midQuality;
        }
        // NOTE: fromEndpointId would be 0 for HighQuality, sice we store only low and mid quality maps.
        outFromEndpointId = 0;
        return highQuality;
    }

    inline bool shouldRecordSsrc(const size_t toEndpointIdHash, const uint32_t ssrc)
    {
        size_t fromEndpointId = 0;
        const auto quality = getCurrentQualityAndEndpointId(ssrc, fromEndpointId);

        // Dominant speaker is always pinned for the recording endpoint.
        const bool fromPinnedEndpoint = _pinMap.end() != _pinMap.find(toEndpointIdHash);
        const auto wantedQuality = fromPinnedEndpoint ? highestActiveQuality(fromEndpointId, ssrc) : lowQuality;

        const auto result = wantedQuality == quality;

        DIRECTOR_LOG("shouldRecordSsrc toEndpointIdHash %lu ssrc %u: result %c, dominant speaker: %c, quality: %lu",
            _loggableId.c_str(),
            toEndpointIdHash,
            ssrc,
            result ? 't' : 'f',
            fromPinnedEndpoint ? 't' : 'f',
            quality);
        return result;
    }

    inline bool shouldForwardSsrc(const size_t toEndpointIdHash, const uint32_t ssrc)
    {
        const auto viewer = _participantStreams.getItem(toEndpointIdHash);

        if (!viewer)
        {
            return false;
        }

        if (isSsrcFromParticipant(toEndpointIdHash, ssrc))
        {
            DIRECTOR_LOG("shouldForwardSsrc toEndpointIdHash %lu ssrc %u: f - own video packet.",
                _loggableId.c_str(),
                toEndpointIdHash,
                ssrc);
            return false;
        }

        // If slides ssrc is checked here, it must've passed isSsrcUsed check for being in the LastN, so
        // forward unconditionally here (even if desired 'unpinned' quality is 'drop').
        if (ssrc == _slidesSsrc)
        {
            DIRECTOR_LOG("shouldForwardSsrc toEndpointIdHash %lu ssrc %u: t - slides.",
                _loggableId.c_str(),
                toEndpointIdHash,
                ssrc);
            return true;
        }

        const auto pinMapItr = _pinMap.find(toEndpointIdHash);
        const bool fromPinnedEndpoint = pinMapItr != _pinMap.end() && isSsrcFromParticipant(pinMapItr->second, ssrc);
        const auto maxWantedQuality =
            (fromPinnedEndpoint ? viewer->desiredHighestEstimatedPinnedLevel : viewer->desiredUnpinnedLevel);

        size_t fromEndpointId = 0;
        size_t quality = getCurrentQualityAndEndpointId(ssrc, fromEndpointId);

        DIRECTOR_LOG("shouldForwardSsrc toEndpointIdHash %lu ssrc %u: cur quality: %lu, wanted quality: %lu",
            _loggableId.c_str(),
            toEndpointIdHash,
            ssrc,
            quality,
            maxWantedQuality);

        // Check against max desired quality.
        bool result = false;
        if (maxWantedQuality == dropQuality)
        {
            result = false;
        }
        else if (quality == maxWantedQuality)
        {
            result = true;
        }
        else if (quality > maxWantedQuality)
        {
            result = false;
        }
        else
        {
            assert(quality != highQuality);
            result = quality == highestActiveQuality(fromEndpointId, ssrc);
        }

        DIRECTOR_LOG("shouldForwardSsrc toEndpointIdHash %lu ssrc %u: result %c, curQ %lu, phaQ %lu, "
                     "wantQ %lu, pinned %c",
            _loggableId.c_str(),
            toEndpointIdHash,
            ssrc,
            result ? 't' : 'f',
            quality,
            highestActiveQuality(fromEndpointId, ssrc),
            maxWantedQuality,
            fromPinnedEndpoint ? 't' : 'f');

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
        auto& primary = participantStreams.primary;
        auto& secondary = participantStreams.secondary;

        logger::info("streamActiveStateChanged, endpointIdHash %lu, ssrc %u, active %c",
            _loggableId.c_str(),
            endpointIdHash,
            ssrc,
            active ? 't' : 'f');

        for (size_t i = 0; i < primary.numLevels; ++i)
        {
            if (ssrc == primary.levels[i].ssrc)
            {
                primary.levels[i].mediaActive = active;
                return setHighestActiveIndex(endpointIdHash, primary);
            }
        }

        if (secondary.isSet())
        {
            for (size_t i = 0; i < secondary.get().numLevels; ++i)
            {
                if (ssrc == secondary.get().levels[i].ssrc)
                {
                    secondary.get().levels[i].mediaActive = active;
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
        auto& primary = participantStreams.primary;
        auto& secondary = participantStreams.secondary;

        SimulcastStream* simulcastStream;
        if (defaultLevelSsrc == primary.levels[lowQuality].ssrc)
        {
            simulcastStream = &primary;
        }
        else if (secondary.isSet() && defaultLevelSsrc == secondary.get().levels[lowQuality].ssrc)
        {
            simulcastStream = &secondary.get();
        }
        else
        {
            assert(false);
            return false;
        }

        outDefaultLevelFeedbackSsrc = simulcastStream->levels[lowQuality].feedbackSsrc;
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
        const auto& primary = participantStreams.primary;
        auto& secondary = participantStreams.secondary;

        for (size_t i = 0; i < primary.numLevels; ++i)
        {
            if (primary.levels[i].feedbackSsrc == feedbackSsrc)
            {
                outSsrc = primary.levels[i].ssrc;
                return true;
            }
        }

        if (secondary.isSet())
        {
            for (size_t i = 0; i < secondary.get().numLevels; ++i)
            {
                if (secondary.get().levels[i].feedbackSsrc == feedbackSsrc)
                {
                    outSsrc = secondary.get().levels[i].ssrc;
                    return true;
                }
            }
        }

        return false;
    }

    void setSlidesSsrcAndBitrate(size_t slidesSsrc, uint32_t bwKbps)
    {
        _slidesSsrc = slidesSsrc;
        _slidesBitrateKbps = bwKbps;
    }

private:
    enum QualityLevel
    {
        lowQuality = 0,
        midQuality = 1,
        highQuality = 2,
        dropQuality = 3
    };

    /** All bandwidth valuea are in kbps. */
    struct ConfigRow
    {
        const size_t BaseRate;
        const size_t PinnedQuality;
        const size_t UnpinnedQuality;
        const size_t OverheadBitrate;
        const size_t MinBitrateMargin;
        const size_t MaxBitrateMargin;
    };

    static const ConfigRow configLadder[6];

    /** Important: This has to be a lot bigger than the actual maximum participants per conference since we have
     * to avoid map entry reuse. Currently multiplied by 2 for that reason. */
    static constexpr size_t maxParticipants = 1024 * 2;
    static const uint64_t timeBeforeScaleUpMs = 5000ULL;

    logger::LoggableId _loggableId;
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

    /** Max number of the video streams forwarded to any particular endpoint. */
    uint32_t _lastN;

    /** Estimated min bandwidth screensharing/slides will obey based on min of all participants uplink estimates. */
    uint32_t _slidesBitrateKbps;

    /** SSRC for slides. */
    size_t _slidesSsrc;

    inline size_t highestActiveQuality(const size_t endpointIdHash, const uint32_t ssrc)
    {
        const auto participantStreamsItr = _participantStreams.find(endpointIdHash);
        if (participantStreamsItr == _participantStreams.end())
        {
            return 0;
        }

        auto& participantStreams = participantStreamsItr->second;
        const auto& primary = participantStreams.primary;
        const auto& secondaryOptional = participantStreams.secondary;

        if (ssrc == primary.levels[0].ssrc || ssrc == primary.levels[1].ssrc || ssrc == primary.levels[2].ssrc)
        {
            return primary.highestActiveLevel;
        }

        if (secondaryOptional.isSet())
        {
            const auto& secondary = secondaryOptional.get();
            if (ssrc == secondary.levels[0].ssrc || ssrc == secondary.levels[1].ssrc ||
                ssrc == secondary.levels[2].ssrc)
            {
                return secondary.highestActiveLevel;
            }
        }

        return dropQuality;
    }

    inline bool isSsrcFromParticipant(const size_t endpointIdHash, const uint32_t ssrc)
    {
        const auto participantStreamsItr = _participantStreams.find(endpointIdHash);
        if (participantStreamsItr == _participantStreams.end())
        {
            return false;
        }
        auto& participantStreams = participantStreamsItr->second;
        const auto& primary = participantStreams.primary;
        auto& secondary = participantStreams.secondary;

        for (size_t i = 0; i < primary.numLevels; ++i)
        {
            if (ssrc == primary.levels[i].ssrc)
            {
                return true;
            }
        }

        if (secondary.isSet())
        {
            for (size_t i = 0; i < secondary.get().numLevels; ++i)
            {
                if (ssrc == secondary.get().levels[i].ssrc)
                {
                    return true;
                }
            }
        }

        return false;
    }

    inline bool setHighestActiveIndex(const size_t endpointIdHash, SimulcastStream& simulcastStream)
    {
        const auto oldHighestActiveIndex = simulcastStream.highestActiveLevel;

        simulcastStream.highestActiveLevel = 0;
        for (auto i = (SimulcastStream::maxLevels - 1); i > 0; --i)
        {
            if (simulcastStream.levels[i].mediaActive)
            {
                simulcastStream.highestActiveLevel = i;
                break;
            }
        }

        if (simulcastStream.highestActiveLevel > oldHighestActiveIndex &&
            _reversePinMap.find(endpointIdHash) != _reversePinMap.end())
        {
            return true;
        }

        return oldHighestActiveIndex != simulcastStream.highestActiveLevel;
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
        return ParticipantStreams(primary, secondary, _maxDefaultLevelBandwidthKbps);
    }

    inline bool isContentSlides(const uint32_t ssrc, const size_t senderEndpointIdHash)
    {
        const auto participantStreamsItr = _participantStreams.find(senderEndpointIdHash);
        if (participantStreamsItr == _participantStreams.end())
        {
            return false;
        }

        const auto& primary = participantStreamsItr->second.primary;

        if (primary.contentType == SimulcastStream::VideoContentType::SLIDES && primary.numLevels == 1 &&
            primary.levels[0].ssrc == ssrc)
        {
            return true;
        }

        if (!participantStreamsItr->second.secondary.isSet())
        {
            return false;
        }
        const auto& secondary = participantStreamsItr->second.secondary.get();

        if (secondary.contentType == SimulcastStream::VideoContentType::SLIDES && secondary.numLevels == 1 &&
            secondary.levels[0].ssrc == ssrc)
        {
            return true;
        }

        return false;
    }

    inline bool anyParticipantsWithoutPinTarget() const { return _participantStreams.size() != _pinMap.size(); }

    /**
     * Checks for ssrcs belonging to a default level if there are participants without pin targets.
     */
    inline bool isUsedForUnpinnedVideo(const size_t quality,
        const size_t highestAvailableQuality,
        const size_t senderEndpointIdHash,
        const bool isSenderInLastNList)
    {
        // Fast return, we serve unpinned only:
        // - when it's low or medium quality;
        // - when the sender is in the LastN list;
        if (quality == highQuality)
        {
            return false;
        }

        if (!isSenderInLastNList)
        {
            return false;
        }

        // Unpinned, belogns to lastN and some of the participants needs this quality.
        for (const auto& participant : _participantStreams)
        {
            if (participant.first != senderEndpointIdHash &&
                quality == std::min(participant.second.desiredUnpinnedLevel, highestAvailableQuality))
            {
                return true;
            }
        }
        // If the sender endpoint is in the lastN list and pinned, we'll return false
        // but for pinned one there is another check in "isSsrcUsed".
        return false;
    }

    inline bool isUsedForPinnedVideo(const size_t quality,
        const size_t highestAvailableQuality,
        const size_t senderEndpointIdHash)
    {
        // Fast return, if nobody pins this endpoint.
        const auto reversePinMapItr = _reversePinMap.find(senderEndpointIdHash);
        if (reversePinMapItr != _reversePinMap.end() && reversePinMapItr->second == 0)
        {
            return false;
        }

        // If somebody pin this endpoint, we need check what quality is actually needed.
        for (const auto& pinMapEntry : _pinMap)
        {
            auto const& pinnedBy = pinMapEntry.first;
            auto const& sender = pinMapEntry.second;
            if (pinnedBy == senderEndpointIdHash || sender != senderEndpointIdHash)
            {
                continue;
            }

            const auto& participant = _participantStreams.find(pinnedBy);

            if (participant != _participantStreams.end() &&
                quality == std::min(participant->second.desiredHighestEstimatedPinnedLevel, highestAvailableQuality))
            {
                return true;
            }
        }
        return false;
    }

    inline bool isUsedForRecordingSlides(const size_t ssrc,
        const size_t senderEndpointIdHash,
        const size_t numRecordingStreams)
    {
        return (numRecordingStreams != 0 && isContentSlides(ssrc, senderEndpointIdHash));
    }

    inline void getVideoQualityLimits(const ParticipantStreams& participantStreams,
        size_t& outPinnedQuality,
        size_t& outUnpinnedQuality) const
    {
        const auto maxVideoStreams = std::min(std::max(1ul, _lowQualitySsrcs.size()), (unsigned long)_lastN - 1);
        if (maxVideoStreams < 1)
        {
            return;
        }

        int bestConfigId = 0;
        unsigned long bestConfigCost = 0;
        int configId = 0;

        const auto estimatedUplinkBandwidth =
            (participantStreams.estimatedUplinkBandwidth != 0 ? participantStreams.estimatedUplinkBandwidth
                                                              : _maxDefaultLevelBandwidthKbps);

        for (const auto& config : configLadder)
        {
            const auto configCost = config.BaseRate + maxVideoStreams * config.OverheadBitrate + _slidesBitrateKbps;

            assert(configCost >= config.MinBitrateMargin + _slidesBitrateKbps);
            assert(configCost <= config.MaxBitrateMargin + _slidesBitrateKbps);

            if (configCost >= bestConfigCost && configCost <= estimatedUplinkBandwidth)
            {
                bestConfigCost = configCost;
                bestConfigId = configId;
            }
            configId++;
        }
        assert(configId == 6);
        outPinnedQuality = configLadder[bestConfigId].PinnedQuality;
        outUnpinnedQuality = configLadder[bestConfigId].UnpinnedQuality;

        DIRECTOR_LOG("VQ pinned: %c, unpinned %c, max streams %ld, estimated uplink %d, reserve for slides: %d",
            _loggableId.c_str(),
            (char)outPinnedQuality + '0',
            (char)outUnpinnedQuality + '0',
            maxVideoStreams,
            estimatedUplinkBandwidth,
            _slidesBitrateKbps);
    }
};

} // namespace bridge
