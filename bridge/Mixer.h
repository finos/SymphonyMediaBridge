#pragma once

#include "bridge/Barbell.h"
#include "bridge/CodecCapabilities.h"
#include "bridge/RecordingStream.h"
#include "bridge/Stats.h"
#include "bridge/engine/ActiveTalker.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/SimulcastLevel.h"
#include "bridge/engine/SsrcRewrite.h"
#include "logger/Logger.h"
#include "transport/Endpoint.h"
#include "transport/dtls/SrtpClient.h"
#include "transport/ice/IceSession.h"
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace api
{
struct RecordingChannel;
struct ConferenceEndpoint;
struct ConferenceEndpointExtendedInfo;
} // namespace api

namespace transport
{
class TransportFactory;
class RtcTransport;
} // namespace transport

namespace utils
{
class IdGenerator;
class SsrcGenerator;
template <typename T>
class Optional;
class SimpleJson;
} // namespace utils

namespace config
{
class Config;
}
namespace bridge
{

class Engine;

struct EngineAudioStream;
struct EngineVideoStream;
struct AudioStreamDescription;
struct VideoStreamDescription;
struct DataStreamDescription;
struct TransportDescription;
struct EngineDataStream;
struct EngineRecordingStream;
class PacketCache;
struct AudioStream;
struct DataStream;
struct RecordingDescription;
struct RtpMap;
struct SimulcastStream;
struct SsrcWhitelist;
struct VideoStream;
struct Barbell;
struct EngineBarbell;

class Mixer
{
public:
    struct Stats
    {
        uint32_t videoStreams = 0;
        uint32_t audioStreams = 0;
        uint32_t dataStreams = 0;
        uint32_t pacingQueue = 0;
        uint32_t rtxPacingQueue = 0;
        uint32_t transports = 0;
    };

    Mixer(std::string id,
        size_t logInstanceId,
        transport::TransportFactory& transportFactory,
        jobmanager::JobManager& backgroundJobQueue,
        std::unique_ptr<EngineMixer> engineMixer,
        utils::IdGenerator& idGenerator,
        utils::SsrcGenerator& ssrcGenerator,
        const config::Config& config,
        const std::vector<uint32_t>& audioSsrcs,
        const std::vector<api::SimulcastGroup>& videoSsrcs,
        const std::vector<api::SsrcPair>& videoPinSsrcs,
        bool useGlobalPort,
        VideoCodecSpec videoCodecs);

    virtual ~Mixer() = default;

    void markForDeletion();
    bool isMarkedForDeletion() const { return _markedForDeletion; }
    void stopTransports();

    bool addBundleTransportIfNeeded(const std::string& endpointId, const ice::IceRole iceRole);

    bool addAudioStream(std::string& outId,
        const std::string& endpointId,
        const utils::Optional<ice::IceRole>& iceRole,
        MediaMode mediaMode,
        utils::Optional<uint32_t> idleTimeoutSeconds = utils::Optional<uint32_t>());
    bool addVideoStream(std::string& outId,
        const std::string& endpointId,
        const utils::Optional<ice::IceRole>& iceRole,
        bool rewriteSsrcs,
        utils::Optional<uint32_t> idleTimeoutSeconds = utils::Optional<uint32_t>());

    bool addBundledAudioStream(std::string& outId,
        const std::string& endpointId,
        MediaMode mediaMode,
        utils::Optional<uint32_t> idleTimeoutSeconds = utils::Optional<uint32_t>());
    bool addBundledVideoStream(std::string& outId,
        const std::string& endpointId,
        const bool ssrcRewrite,
        utils::Optional<uint32_t> idleTimeoutSeconds = utils::Optional<uint32_t>());
    bool addBundledDataStream(std::string& outId,
        const std::string& endpointId,
        utils::Optional<uint32_t> idleTimeoutSeconds = utils::Optional<uint32_t>());

    bool addBarbell(const std::string& barbellId, ice::IceRole iceRole);

    bool removeAudioStream(const std::string& endpointId);
    bool removeAudioStreamId(const std::string& id);

    bool removeVideoStream(const std::string& endpointId);
    bool removeVideoStreamId(const std::string& id);

    bool removeDataStream(const std::string& endpointId);
    bool removeDataStreamId(const std::string& id);

    void engineAudioStreamRemoved(const EngineAudioStream& engineStream);
    void engineVideoStreamRemoved(const EngineVideoStream& engineStream);
    void engineDataStreamRemoved(const EngineDataStream& engineStream);
    void engineBarbellRemoved(const EngineBarbell& engineBarbell);

    bool configureAudioStream(const std::string& endpointId,
        const RtpMap& rtpMap,
        const RtpMap& telephoneEventRtpMap,
        const utils::Optional<uint32_t>& remoteSsrc,
        const std::vector<uint32_t>& neighbours);

    bool reconfigureAudioStream(const std::string& endpointId, const utils::Optional<uint32_t>& remoteSsrc);
    bool reconfigureAudioStreamNeighbours(const std::string& endpointId, const std::vector<uint32_t>& neighbours);

    bool configureVideoStream(const std::string& endpointId,
        const RtpMap& rtpMap,
        const RtpMap& feedbackRtpMap,
        const SimulcastStream& simulcastStream,
        const utils::Optional<SimulcastStream>& secondarySimulcastStream,
        const SsrcWhitelist& ssrcWhitelist);

    bool reconfigureVideoStream(const std::string& endpointId,
        const SimulcastStream& simulcastStream,
        const utils::Optional<SimulcastStream>& secondarySimulcastStream,
        const SsrcWhitelist& ssrcWhitelist);

    bool configureDataStream(const std::string& endpointId, const uint32_t sctpPort);

    bool configureAudioStreamTransportIce(const std::string& endpointId,
        const std::pair<std::string, std::string>& credentials,
        const ice::IceCandidates& candidates);

    bool configureVideoStreamTransportIce(const std::string& endpointId,
        const std::pair<std::string, std::string>& credentials,
        const ice::IceCandidates& candidates);

    bool configureAudioStreamTransportConnection(const std::string& endpointId, const transport::SocketAddress& peer);
    bool configureVideoStreamTransportConnection(const std::string& endpointId, const transport::SocketAddress& peer);

    bool configureAudioStreamTransportDtls(const std::string& endpointId,
        const std::string& fingerprintType,
        const std::string& fingerprintHash,
        const bool isDtlsClient);
    bool configureAudioStreamTransportSdes(const std::string& endpointId, const srtp::AesKey& remoteKey);
    bool configureAudioStreamTransportDisableSrtp(const std::string& endpointId);

    bool configureVideoStreamTransportDtls(const std::string& endpointId,
        const std::string& fingerprintType,
        const std::string& fingerprintHash,
        const bool isDtlsClient);
    bool configureVideoStreamTransportSdes(const std::string& endpointId, const srtp::AesKey& remoteKey);
    bool configureVideoStreamTransportDisableSrtp(const std::string& endpointId);

    bool configureBundleTransportIce(const std::string& endpointId,
        const std::pair<std::string, std::string>& credentials,
        const ice::IceCandidates& candidates);

    bool configureBundleTransportDtls(const std::string& endpointId,
        const std::string& fingerprintType,
        const std::string& fingerprintHash,
        const bool isDtlsClient);
    bool configureBundleTransportSdes(const std::string& endpointId, const srtp::AesKey& remoteKey);

    bool configureBarbellTransport(const std::string& barbellId,
        const std::pair<std::string, std::string>& credentials,
        const ice::IceCandidates& candidates,
        const std::string& fingerprintType,
        const std::string& fingerprintHash,
        const bool isDtlsClient);
    bool configureBarbellSsrcs(const std::string& barbellId,
        const std::vector<BarbellVideoStreamDescription>& videoSsrcs,
        const std::vector<uint32_t>& audioSsrcs,
        const bridge::RtpMap& audioRtpMap,
        const bridge::RtpMap& videoRtpMap,
        const bridge::RtpMap& videoFeedbackRtpMap);

    bool addBarbellToEngine(const std::string& barbellId);
    bool startBarbellTransport(const std::string& barbellId);
    void removeBarbell(const std::string& barbellId);

    bool pinEndpoint(const size_t endpointIdHash, const char* pinnedEndpointId);
    bool unpinEndpoint(const size_t endpointIdHash);

    bool startAudioStreamTransport(const std::string& endpointId);
    bool startVideoStreamTransport(const std::string& endpointId);
    bool startBundleTransport(const std::string& endpointId);

    bool addAudioStreamToEngine(const std::string& endpointId);
    bool addVideoStreamToEngine(const std::string& endpointId);
    bool addDataStreamToEngine(const std::string& endpointId);
    bool isAudioStreamGatheringComplete(const std::string& endpointId);
    bool isVideoStreamGatheringComplete(const std::string& endpointId);
    bool isDataStreamGatheringComplete(const std::string& endpointId);

    const std::string getId() const { return _id; }

    const logger::LoggableId& getLoggableId() const { return _loggableId; }

    bool getEndpointInfo(const std::string& endpointId,
        api::ConferenceEndpoint&,
        const std::map<size_t, ActiveTalker>& activeTalkers);
    bool getEndpointExtendedInfo(const std::string& endpointId,
        api::ConferenceEndpointExtendedInfo&,
        const std::map<size_t, ActiveTalker>& activeTalkers);
    std::map<size_t, ActiveTalker> getActiveTalkers();
    bool getAudioStreamDescription(const std::string& endpointId, AudioStreamDescription& outDescription);
    bool getVideoStreamDescription(const std::string& endpointId, VideoStreamDescription& outDescription);
    bool getDataStreamDescription(const std::string& endpointId, DataStreamDescription& outDescription);
    bool isAudioStreamConfigured(const std::string& endpointId);
    bool isVideoStreamConfigured(const std::string& endpointId);
    bool isDataStreamConfigured(const std::string& endpointId);

    void getAudioStreamDescription(AudioStreamDescription& outDescription);
    void getBarbellVideoStreamDescription(std::vector<BarbellVideoStreamDescription>& outDescription);

    bool getTransportBundleDescription(const std::string& endpointId, TransportDescription& outTransportDescription);
    bool getAudioStreamTransportDescription(const std::string& endpointId,
        TransportDescription& outTransportDescription);
    bool getVideoStreamTransportDescription(const std::string& endpointId,
        TransportDescription& outTransportDescription);

    bool getBarbellTransportDescription(const std::string& barbellId, TransportDescription& outTransportDescription);

    void allocateVideoPacketCache(const uint32_t ssrc, const size_t endpointIdHash);
    void freeVideoPacketCache(const uint32_t ssrc, const size_t endpointIdHash);

    bool addOrUpdateRecording(const std::string& conferenceId,
        const std::vector<api::RecordingChannel>& channels,
        const RecordingDescription& recordingDescription);
    void addRecordingTransportsToRecordingStream(RecordingStream& recordingStream,
        const std::vector<api::RecordingChannel>& channels);
    void updateRecordingEngineStreamModalities(const RecordingStream& recordingStream,
        const bool wasAudioEnabled,
        const bool wasVideoEnabled,
        const bool wasScreenSharingEnabled);
    bool removeRecording(const std::string& recordingId);
    bool removeRecordingTransports(const std::string& conferenceId, const std::vector<api::RecordingChannel>& channels);
    void engineRecordingStreamRemoved(const EngineRecordingStream& engineStream);
    void engineRecordingDescStopped(const RecordingDescription& recordingDesc);
    void allocateRecordingRtpPacketCache(const uint32_t ssrc, const size_t endpointIdHash);
    void freeRecordingRtpPacketCache(const uint32_t ssrc, const size_t endpointIdHash);
    void removeRecordingTransport(const std::string& streamId, const size_t endpointIdHash);

    Stats getStats();
    bool hasPendingTransportJobs();

    void sendEndpointMessage(const std::string& toEndpointId,
        const size_t fromEndpointIdHash,
        const utils::SimpleJson& message);

    std::unordered_set<std::string> getEndpoints() const;
    EngineAudioStream* getEngineAudioStream(const std::string& endpointId);
    EngineVideoStream* getEngineVideoStream(const std::string& endpointId);
    EngineDataStream* getEngineDataStream(const std::string& endpointId);

    EngineMixer* getEngineMixer() { return _engineMixer.get(); }

    const config::Config& getConfig() const { return _config; }
    bridge::Stats::MixerBarbellStats gatherBarbellStats(const uint64_t engineIterationStartTimestamp);

    bool isH264Enabled() const;

private:
    struct BundleTransport
    {
        explicit BundleTransport(const std::shared_ptr<transport::RtcTransport>& transport) : transport(transport) {}
        std::shared_ptr<transport::RtcTransport> transport;
        srtp::Mode srtpMode;
    };

    const config::Config& _config;
    const std::string _id;
    logger::LoggableId _loggableId;
    bool _markedForDeletion;

    const std::vector<uint32_t> _audioSsrcs;
    const std::vector<api::SimulcastGroup> _videoSsrcs;
    const std::vector<api::SsrcPair> _videoPinSsrcs;

    transport::TransportFactory& _transportFactory;
    jobmanager::JobManager& _backgroundJobQueue;
    std::unique_ptr<EngineMixer> _engineMixer;
    utils::IdGenerator& _idGenerator;
    utils::SsrcGenerator& _ssrcGenerator;

    std::unordered_map<std::string, std::unique_ptr<AudioStream>> _audioStreams;
    std::unordered_map<std::string, std::unique_ptr<EngineAudioStream>> _audioEngineStreams;
    std::unordered_map<std::string, std::unique_ptr<VideoStream>> _videoStreams;
    std::unordered_map<std::string, std::unique_ptr<EngineVideoStream>> _videoEngineStreams;
    std::unordered_map<std::string, std::unique_ptr<DataStream>> _dataStreams;
    std::unordered_map<std::string, std::unique_ptr<EngineDataStream>> _dataEngineStreams;
    std::unordered_map<std::string, std::unique_ptr<RecordingStream>> _recordingStreams;
    std::unordered_map<std::string, std::unique_ptr<EngineRecordingStream>> _recordingEngineStreams;

    std::unordered_map<std::string, BundleTransport> _bundleTransports;
    bool _useGlobalPort;
    const VideoCodecSpec _videoCodecs;
    transport::Endpoints _rtpPorts;
    std::unordered_map<size_t, std::unordered_map<uint32_t, std::unique_ptr<PacketCache>>> _videoPacketCaches;
    std::unordered_map<size_t, std::unordered_map<uint32_t, std::unique_ptr<PacketCache>>> _recordingRtpPacketCaches;
    std::unordered_map<size_t, std::unique_ptr<PacketCache>> _recordingEventPacketCache;

    std::unordered_map<std::string, std::unique_ptr<Barbell>> _barbells;
    std::unordered_map<std::string, std::unique_ptr<EngineBarbell>> _engineBarbells;
    transport::Endpoints _barbellPorts;

    std::mutex _configurationLock;

    RecordingStream* findRecordingStream(const std::string& recordingId);

    void stopTransportIfNeeded(const std::shared_ptr<transport::RtcTransport>& streamTransport,
        const std::string& endpointId);
    bridge::Stats::BarbellPayloadStats fromPacketCounter(const transport::PacketCounters& counters);
};

} // namespace bridge
