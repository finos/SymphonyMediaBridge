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
    static const uint32_t LOW_QUALITY_BITRATE;
    static const uint32_t MID_QUALITY_BITRATE;
    static const uint32_t HIGH_QUALITY_BITRATE;

    enum QualityLevel : uint32_t
    {
        lowQuality = 0,
        midQuality = 1,
        highQuality = 2,
        dropQuality = 3
    };

    struct ParticipantStreams
    {
        ParticipantStreams(const SimulcastStream& primary,
            const utils::Optional<SimulcastStream>& secondary,
            const uint32_t maxDefaultLevelBandwidthKbps)
            : primary(primary),
              secondary(secondary),
              pinQualityLevel(static_cast<QualityLevel>(SimulcastStream::maxLevels - 1)),
              unpinQualityLevel(lowQuality),
              lowEstimateTimestamp(lowQuality),
              defaultLevelBandwidthLimit(maxDefaultLevelBandwidthKbps),
              estimatedUplinkBandwidth(0)
        {
        }
        SimulcastStream primary;
        utils::Optional<SimulcastStream> secondary;
        QualityLevel pinQualityLevel;
        QualityLevel unpinQualityLevel;
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
          _slidesBitrateKbps(0),
          _slidesSsrc(0)
    {
    }

    EngineStreamDirector()
        : _loggableId("StreamDirector-empty"),
          _participantStreams(0),
          _pinMap(0),
          _reversePinMap(0),
          _lowQualitySsrcs(0),
          _midQualitySsrcs(0),
          _bandwidthFloor(0),
          _requiredMidLevelBandwidth(0),
          _maxDefaultLevelBandwidthKbps(0),
          _lastN(0),
          _slidesBitrateKbps(0),
          _slidesSsrc(0)
    {
    }

    void addParticipant(const size_t endpointIdHash)
    {
        if (_participantStreams.find(endpointIdHash) != _participantStreams.end())
        {
            DIRECTOR_LOG("addParticipant stream already added, endpointIdHash %lu",
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
            DIRECTOR_LOG("addParticipant stream already added, endpointIdHash %lu",
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

        if (primary.isSendingSlides())
        {
            // Set bwKbps, this will force EngineMixer to update bwKbps on next iteration
            setSlidesSsrcAndBitrate(primary.levels[0].ssrc, 0);
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

            if (secondary->isSendingSlides())
            {
                // Set bwKbps, this will force EngineMixer to update bwKbps on next iteration
                setSlidesSsrcAndBitrate(secondary->levels[0].ssrc, 0);
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

            if (participantStream.primary.isSendingSlides())
            {
                _slidesBitrateKbps = 0;
            }
        }
        if (participantStream.secondary.isSet() && participantStream.secondary.get().numLevels > 0)
        {
            _lowQualitySsrcs.erase(participantStream.secondary.get().levels[lowQuality].ssrc);
            _midQualitySsrcs.erase(participantStream.secondary.get().levels[midQuality].ssrc);
            assert(_requiredMidLevelBandwidth > 0);
            _requiredMidLevelBandwidth -= bwe::BandwidthUtils::getSimulcastLevelKbps(midQuality);

            if (participantStream.secondary.get().isSendingSlides())
            {
                _slidesBitrateKbps = 0;
            }
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
        if (_participantStreams.capacity())
        {
            _bandwidthFloor = bwe::BandwidthUtils::calcBandwidthFloor(lowQuality, lastN, audioStreams, videoStreams);
            logger::debug("updateBandwidthFloor lastN %u, audioStreams %u, videoStreams %u -> %u",
                _loggableId.c_str(),
                lastN,
                audioStreams,
                videoStreams,
                _bandwidthFloor);
        }
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

        QualityLevel desiredPinQuality, unpinnedQuality;
        getVideoQualityLimits(endpointIdHash, participantStream, desiredPinQuality, unpinnedQuality);

        participantStream.unpinQualityLevel = unpinnedQuality;

        if (desiredPinQuality == participantStream.pinQualityLevel)
        {
            participantStream.lowEstimateTimestamp = timestamp;
            return false;
        }
        else if (desiredPinQuality < participantStream.pinQualityLevel)
        {
            logger::info("setUplinkEstimateKbps %u, endpointIdHash %zu, degrade video pin %u -> %u, unpinned %u",
                _loggableId.c_str(),
                uplinkEstimateKbps,
                endpointIdHash,
                participantStream.pinQualityLevel,
                desiredPinQuality,
                participantStream.unpinQualityLevel);

            participantStream.pinQualityLevel = desiredPinQuality;
            participantStream.lowEstimateTimestamp = timestamp;
            return true;
        }

        if (utils::Time::diffGE(participantStream.lowEstimateTimestamp,
                timestamp,
                timeBeforeScaleUpMs * utils::Time::ms))
        {
            logger::info("setUplinkEstimateKbps %u, endpointIdHash %zu, upgrade video pin %u -> %u, unpinned %u",
                _loggableId.c_str(),
                uplinkEstimateKbps,
                endpointIdHash,
                participantStream.pinQualityLevel,
                desiredPinQuality,
                participantStream.unpinQualityLevel);

            participantStream.pinQualityLevel = desiredPinQuality;
            participantStream.lowEstimateTimestamp = timestamp;
            return true;
        }
        else
        {
            logger::debug("setUplinkEstimateKbps %u, endpointIdHash %zu, want pin quality change %u -> %u",
                _loggableId.c_str(),
                uplinkEstimateKbps,
                endpointIdHash,
                participantStream.pinQualityLevel,
                desiredPinQuality);
        }

        return false;
    }

    inline QualityLevel getQualityLevel(const uint32_t ssrc)
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
    inline bool isSsrcUsed(uint32_t ssrc,
        const size_t senderEndpointIdHash,
        const bool hasRecentActivity,
        const bool isSenderInLastNList,
        const size_t numRecordingStreams)
    {
        auto participantsStreams = _participantStreams.getItem(senderEndpointIdHash);
        if (!participantsStreams)
        {
            DIRECTOR_LOG("isSsrcUsed, %u false, endpoint removed", _loggableId.c_str(), ssrc);
            return false;
        }

        auto mainSsrc = participantsStreams->primary.getMainSsrcFor(ssrc);
        if (!mainSsrc.isSet() && participantsStreams->secondary.isSet())
        {
            mainSsrc = participantsStreams->secondary.get().getMainSsrcFor(ssrc);
        }

        if (mainSsrc.isSet())
        {
            ssrc = mainSsrc.get();
        }

        const auto quality = getQualityLevel(ssrc);
        const auto highestAvailableQuality = hasRecentActivity
            ? std::max(highestActiveQuality(senderEndpointIdHash, ssrc), quality)
            : highestActiveQuality(senderEndpointIdHash, ssrc);

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
            DIRECTOR_LOG("isSsrcUsed isContentSlides %u: result f", _loggableId.c_str(), ssrc);
            return true;
        }

        DIRECTOR_LOG("isSsrcUsed, %u false", _loggableId.c_str(), ssrc);
        return false;
    }

    bool isPinned(size_t endpointIdHash) const { return _reversePinMap.contains(endpointIdHash); }

    inline QualityLevel getCurrentQualityAndEndpointId(const uint32_t ssrc, size_t& outFromEndpointId)
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
        // NOTE: fromEndpointId would be 0 for HighQuality, since we store only low and mid quality maps.
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

        DIRECTOR_LOG("shouldRecordSsrc toEndpointIdHash %lu ssrc %u: result %c, dominant speaker: %c, quality: %u",
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

        // In case slides are used - limit video resolution to save CPU on recipients.
        const auto assignedQuality =
            (fromPinnedEndpoint && 0 == _slidesBitrateKbps) ? viewer->pinQualityLevel : viewer->unpinQualityLevel;

        size_t fromEndpointId = 0;
        const auto quality = getCurrentQualityAndEndpointId(ssrc, fromEndpointId);

        DIRECTOR_LOG("shouldForwardSsrc toEndpointIdHash %zu ssrc %u: cur quality: %u, wanted quality: %u",
            _loggableId.c_str(),
            toEndpointIdHash,
            ssrc,
            quality,
            assignedQuality);

        // Check against max desired quality.
        bool result = false;
        if (assignedQuality == dropQuality)
        {
            result = false;
        }
        else if (quality == assignedQuality)
        {
            result = true;
        }
        else if (quality > assignedQuality)
        {
            result = false;
        }
        else
        {
            assert(quality != highQuality);
            result = quality == highestActiveQuality(fromEndpointId, ssrc);
        }

        DIRECTOR_LOG("shouldForwardSsrc toEndpointIdHash %zu ssrc %u: result %c, curQ %u, phaQ %u, "
                     "wantQ %u, pinned %c",
            _loggableId.c_str(),
            toEndpointIdHash,
            ssrc,
            result ? 't' : 'f',
            quality,
            highestActiveQuality(fromEndpointId, ssrc),
            assignedQuality,
            fromPinnedEndpoint ? 't' : 'f');

        return result;
    }

    /**
     * This is called in parallel with add/remove. This is ok as long as the _participantStreams map has a lot of spare
     * space. Since this function will possibly access elements after removal. MpmcMap does not return memory for
     * removed elements to the OS and it does not reuse removed elements until all other free elements are reused.
     */
    void streamActiveStateChanged(const size_t endpointIdHash, const uint32_t ssrc, const bool active)
    {
        auto participantStreamsItr = _participantStreams.find(endpointIdHash);
        if (participantStreamsItr == _participantStreams.end())
        {
            return;
        }
        auto& participantStreams = participantStreamsItr->second;
        auto& primary = participantStreams.primary;
        auto& secondary = participantStreams.secondary;

        logger::info("streamActiveStateChanged, endpointIdHash %lu, ssrc %u, active %c",
            _loggableId.c_str(),
            endpointIdHash,
            ssrc,
            active ? 't' : 'f');

        for (auto& simulcastLevel : primary.getLevels())
        {
            if (ssrc == simulcastLevel.ssrc)
            {
                simulcastLevel.mediaActive = active;
                setHighestActiveIndex(endpointIdHash, primary);
                return;
            }
        }

        if (secondary.isSet())
        {
            for (auto& simulcastLevel : secondary.get().getLevels())
            {
                if (ssrc == simulcastLevel.ssrc)
                {
                    simulcastLevel.mediaActive = active;
                    setHighestActiveIndex(endpointIdHash, secondary.get());
                    return;
                }
            }
        }
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

        for (auto& simulcastLevel : primary.getLevels())
        {
            if (simulcastLevel.feedbackSsrc == feedbackSsrc)
            {
                outSsrc = simulcastLevel.ssrc;
                return true;
            }
        }

        if (secondary.isSet())
        {
            for (auto& simulcastLevel : secondary.get().getLevels())
            {
                if (simulcastLevel.feedbackSsrc == feedbackSsrc)
                {
                    outSsrc = simulcastLevel.ssrc;
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

    bool needsSlidesBitrateAllocation() const { return _slidesSsrc != 0 && _slidesBitrateKbps == 0; }

    uint32_t getBitrateForAllThumbnails() const
    {
        const uint32_t slidesCount = _slidesSsrc == 0 ? 0 : 1;
        return (std::max(slidesCount, std::min(_lastN, static_cast<uint32_t>(_lowQualitySsrcs.size()))) - slidesCount) *
            LOW_QUALITY_BITRATE;
    }

private:
    /** All bandwidth values are in kbps. */
    struct ConfigRow
    {
        const size_t baseRate;
        const QualityLevel pinnedQuality;
        const QualityLevel unpinnedQuality;
        const size_t overheadBitrate;
        const size_t minBitrateMargin;
        const size_t maxBitrateMargin;
    };

    static const ConfigRow configLadder[6];

    /** Important: This has to be a lot bigger than the actual maximum participants per conference since we have
     * to avoid map entry reuse. Currently multiplied by 2 for that reason. */
    static constexpr size_t maxParticipants = 1024 * 2;
    static const uint64_t timeBeforeScaleUpMs = 5000ULL;

    logger::LoggableId _loggableId;
    concurrency::MpmcHashmap32<size_t, ParticipantStreams> _participantStreams;
    concurrency::MpmcHashmap32<size_t, size_t> _pinMap; // less to iterate if fewer use pin
    concurrency::MpmcHashmap32<size_t, size_t> _reversePinMap; // count pinning users
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

    inline QualityLevel highestActiveQuality(const size_t endpointIdHash, const uint32_t ssrc)
    {
        const auto participantStreamsItr = _participantStreams.find(endpointIdHash);
        if (participantStreamsItr == _participantStreams.end())
        {
            return dropQuality;
        }

        auto& participantStreams = participantStreamsItr->second;
        const auto& primary = participantStreams.primary;
        const auto& secondaryOptional = participantStreams.secondary;

        if (ssrc == primary.levels[0].ssrc || ssrc == primary.levels[1].ssrc || ssrc == primary.levels[2].ssrc)
        {
            return static_cast<QualityLevel>(primary.highestActiveLevel);
        }

        if (secondaryOptional.isSet())
        {
            const auto& secondary = secondaryOptional.get();
            if (ssrc == secondary.levels[0].ssrc || ssrc == secondary.levels[1].ssrc ||
                ssrc == secondary.levels[2].ssrc)
            {
                return static_cast<QualityLevel>(secondary.highestActiveLevel);
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

        for (auto& simulcastLevel : primary.getLevels())
        {
            if (ssrc == simulcastLevel.ssrc)
            {
                return true;
            }
        }

        if (secondary.isSet())
        {
            for (auto& simulcastLevel : secondary.get().getLevels())
            {
                if (ssrc == simulcastLevel.ssrc)
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

    /**
     * Checks for ssrcs belonging to a default level if there are participants without pin targets.
     */
    inline bool isUsedForUnpinnedVideo(const size_t quality,
        const QualityLevel highestAvailableQuality,
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

        // Unpinned, belongs to lastN and some of the participants needs this quality.
        // TODO this is too heavy in large conference. A subscription count matrix should solve this
        for (const auto& participant : _participantStreams)
        {
            if (participant.first != senderEndpointIdHash &&
                quality == std::min(participant.second.unpinQualityLevel, highestAvailableQuality))
            {
                return true;
            }
        }
        // If the sender endpoint is in the lastN list and pinned, we'll return false
        // but for pinned one there is another check in "isSsrcUsed".
        return false;
    }

    inline bool isUsedForPinnedVideo(const QualityLevel quality,
        const QualityLevel highestAvailableQuality,
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
            auto const& pinTarget = pinMapEntry.second;
            if (pinnedBy == senderEndpointIdHash || pinTarget != senderEndpointIdHash)
            {
                continue;
            }

            const auto& participant = _participantStreams.find(pinnedBy);

            if (participant != _participantStreams.end() &&
                quality == std::min(participant->second.pinQualityLevel, highestAvailableQuality))
            {
                return true;
            }
        }
        return false;
    }

    inline bool isUsedForRecordingSlides(const uint32_t ssrc,
        const size_t senderEndpointIdHash,
        const size_t numRecordingStreams)
    {
        return (numRecordingStreams != 0 && isContentSlides(ssrc, senderEndpointIdHash));
    }

    inline void getVideoQualityLimits(const size_t endpointIdHash,
        const ParticipantStreams& participantStreams,
        QualityLevel& outPinnedQuality,
        QualityLevel& outUnpinnedQuality) const
    {
        outPinnedQuality = dropQuality;
        outUnpinnedQuality = dropQuality;

        // We need to divide available bitrate (minus bitrate for slides, if present) to "maxReceivingVideoStreams".
        // "maxReceivingVideoStreams" can be 0, if we are the only one sending video, or the very first one in that case
        // quality limits will be initially overestimated (but would be periodically updated with each uplink estimation
        // anyway). To lookup "configLadder" we need at least one stream, thus capping to 1 from below.
        // We will also not include the _slidesBitrateKbps on the cost calculation for the participant that is sending
        // the slides

        const bool sendingVideo = participantStreams.primary.isSendingVideo() ||
            (participantStreams.secondary.isSet() && participantStreams.secondary.get().isSendingVideo());

        const bool sendingSlides = participantStreams.primary.isSendingSlides() ||
            (participantStreams.secondary.isSet() && participantStreams.secondary.get().isSendingSlides());

        const auto maxReceivingVideoStreams = std::max(1ul,
            std::min(std::max(1ul, _lowQualitySsrcs.size()), (unsigned long)_lastN) - (sendingVideo ? 1 : 0));

        int bestConfigId = 0;
        unsigned long bestConfigCost = 0;
        int configId = 0;

        const auto estimatedUplinkBandwidth =
            (participantStreams.estimatedUplinkBandwidth != 0 ? participantStreams.estimatedUplinkBandwidth
                                                              : _maxDefaultLevelBandwidthKbps);

        const uint32_t allocationBitrateKbpsForSlides = (sendingSlides ? 0 : _slidesBitrateKbps);

        for (const auto& config : configLadder)
        {
            const auto configCost =
                config.baseRate + maxReceivingVideoStreams * config.overheadBitrate + allocationBitrateKbpsForSlides;

            assert(configCost >= config.minBitrateMargin + allocationBitrateKbpsForSlides);
            assert(configCost <= config.maxBitrateMargin + allocationBitrateKbpsForSlides);

            const auto configIsBetter = bestConfigCost == 0 || configCost > bestConfigCost;
            if (configIsBetter && configCost <= estimatedUplinkBandwidth)
            {
                bestConfigCost = configCost;
                bestConfigId = configId;
            }
            configId++;
        }
        assert(configId == 6);
        outPinnedQuality = configLadder[bestConfigId].pinnedQuality;
        outUnpinnedQuality = configLadder[bestConfigId].unpinnedQuality;

        DIRECTOR_LOG("VQ pinned: %c, unpinned %c, max streams %ld, estimated uplink %d, reserve for slides: %d "
                     "(endpoint %zu)",
            _loggableId.c_str(),
            (char)outPinnedQuality + '0',
            (char)outUnpinnedQuality + '0',
            maxReceivingVideoStreams,
            estimatedUplinkBandwidth,
            _slidesBitrateKbps,
            endpointIdHash);
    }
};

} // namespace bridge
