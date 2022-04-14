#pragma once

#include "bridge/RecordingStream.h"
#include "bridge/engine/SimulcastLevel.h"
#include "logger/Logger.h"
#include "transport/ice/IceSession.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace api
{
struct RecordingChannel;
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
} // namespace utils

namespace config
{
class Config;
}
namespace bridge
{

class Engine;
class EngineMixer;
struct EngineAudioStream;
struct EngineVideoStream;
struct StreamDescription;
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

class Mixer
{
public:
    struct Stats
    {
        uint32_t _videoStreams = 0;
        uint32_t _audioStreams = 0;
        uint32_t _dataStreams = 0;
        uint32_t pacingQueue = 0;
        uint32_t rtxPacingQueue = 0;
    };

    Mixer(std::string id,
        transport::TransportFactory& transportFactory,
        Engine& engine,
        EngineMixer& engineMixer,
        utils::IdGenerator& idGenerator,
        utils::SsrcGenerator& ssrcGenerator,
        const config::Config& config,
        const std::vector<uint32_t>& audioSsrcs,
        const std::vector<SimulcastLevel>& videoSsrcs,
        const std::vector<SimulcastLevel>& videoPinSsrcs);

    ~Mixer() = default;

    void markForDeletion();
    bool isMarkedForDeletion() const { return _markedForDeletion; }
    void stopTransports();

    bool addBundleTransportIfNeeded(const std::string& endpointId, const ice::IceRole iceRole);

    bool addAudioStream(std::string& outId,
        const std::string& endpointId,
        const utils::Optional<ice::IceRole>& iceRole,
        const bool audioMixed,
        bool rewriteSsrcs);
    bool addVideoStream(std::string& outId,
        const std::string& endpointId,
        const utils::Optional<ice::IceRole>& iceRole,
        bool rewriteSsrcs);

    bool addBundledAudioStream(std::string& outId,
        const std::string& endpointId,
        const bool audioMixed,
        const bool ssrcRewrite);
    bool addBundledVideoStream(std::string& outId, const std::string& endpointId, const bool ssrcRewrite);
    bool addBundledDataStream(std::string& outId, const std::string& endpointId);

    bool removeAudioStream(const std::string& endpointId);
    bool removeAudioStreamId(const std::string& id);

    bool removeVideoStream(const std::string& endpointId);
    bool removeVideoStreamId(const std::string& id);

    bool removeDataStream(const std::string& endpointId);
    bool removeDataStreamId(const std::string& id);

    void engineAudioStreamRemoved(EngineAudioStream* engineStream);
    void engineVideoStreamRemoved(EngineVideoStream* engineStream);
    void engineDataStreamRemoved(EngineDataStream* engineStream);

    bool configureAudioStream(const std::string& endpointId,
        const RtpMap& rtpMap,
        const utils::Optional<uint32_t>& remoteSsrc,
        const utils::Optional<uint8_t>& audioLevelExtensionId,
        const utils::Optional<uint8_t>& absSendTimeExtensionId);

    bool reconfigureAudioStream(const std::string& endpointId, const utils::Optional<uint32_t>& remoteSsrc);

    bool configureVideoStream(const std::string& endpointId,
        const RtpMap& rtpMap,
        const RtpMap& feedbackRtpMap,
        const SimulcastStream& simulcastStream,
        const utils::Optional<SimulcastStream>& secondarySimulcastStream,
        const utils::Optional<uint8_t>& absSendTimeExtensionId,
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

    bool configureVideoStreamTransportDtls(const std::string& endpointId,
        const std::string& fingerprintType,
        const std::string& fingerprintHash,
        const bool isDtlsClient);

    bool configureAudioStreamTransportDisableDtls(const std::string& endpointId);
    bool configureVideoStreamTransportDisableDtls(const std::string& endpointId);

    bool configureBundleTransportIce(const std::string& endpointId,
        const std::pair<std::string, std::string>& credentials,
        const ice::IceCandidates& candidates);

    bool configureBundleTransportDtls(const std::string& endpointId,
        const std::string& fingerprintType,
        const std::string& fingerprintHash,
        const bool isDtlsClient);

    bool pinEndpoint(const size_t endpointIdHash, const std::string& pinnedEndpointId);
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

    bool getAudioStreamDescription(const std::string& endpointId, StreamDescription& outDescription);
    bool getVideoStreamDescription(const std::string& endpointId, StreamDescription& outDescription);
    bool getDataStreamDescription(const std::string& endpointId, DataStreamDescription& outDescription);
    bool isAudioStreamConfigured(const std::string& endpointId);
    bool isVideoStreamConfigured(const std::string& endpointId);
    bool isDataStreamConfigured(const std::string& endpointId);

    bool getTransportBundleDescription(const std::string& endpointId, TransportDescription& outTransportDescription);
    bool getAudioStreamTransportDescription(const std::string& endpointId,
        TransportDescription& outTransportDescription);
    bool getVideoStreamTransportDescription(const std::string& endpointId,
        TransportDescription& outTransportDescription);

    void allocateVideoPacketCache(const uint32_t ssrc, const size_t endpointIdHash);
    void freeVideoPacketCache(const uint32_t ssrc, const size_t endpointIdHash);

    bool addOrUpdateRecording(const std::string& conferenceId,
        const std::vector<api::RecordingChannel>& channels,
        const RecordingDescription& recordingDescription);
    void addRecordingTransportsToRecordingStream(RecordingStream* recordingStream,
        const std::vector<api::RecordingChannel>& channels);
    void updateRecordingEngineStreamModalities(const RecordingStream& recordingStream,
        const bool wasAudioEnabled,
        const bool wasVideoEnabled,
        const bool wasScreenSharingEnabled);
    bool removeRecording(const std::string& recordingId);
    bool removeRecordingTransports(const std::string& conferenceId, const std::vector<api::RecordingChannel>& channels);
    void engineRecordingStreamRemoved(EngineRecordingStream* engineStream);
    void engineRecordingDescStopped(const RecordingDescription& recordingDesc);
    void allocateRecordingRtpPacketCache(const uint32_t ssrc, const size_t endpointIdHash);
    void freeRecordingRtpPacketCache(const uint32_t ssrc, const size_t endpointIdHash);
    void removeRecordingTransport(const std::string& streamId, const size_t endpointIdHash);

    Stats getStats();
    bool waitForAllPendingJobs(const uint32_t timeoutMs);

    void sendEndpointMessage(const std::string& toEndpointId,
        const size_t fromEndpointIdHash,
        const std::string& message);

private:
    struct BundleTransport
    {
        explicit BundleTransport(const std::shared_ptr<transport::RtcTransport>& transport) : _transport(transport) {}
        std::shared_ptr<transport::RtcTransport> _transport;
    };

    const config::Config& _config;
    const std::string _id;
    logger::LoggableId _loggableId;
    bool _markedForDeletion;

    const std::vector<uint32_t> _audioSsrcs;
    const std::vector<SimulcastLevel> _videoSsrcs;
    const std::vector<SimulcastLevel> _videoPinSsrcs;

    transport::TransportFactory& _transportFactory;
    Engine& _engine;
    EngineMixer& _engineMixer;
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
    std::unordered_map<size_t, std::unordered_map<uint32_t, std::unique_ptr<PacketCache>>> _videoPacketCaches;
    std::unordered_map<size_t, std::unordered_map<uint32_t, std::unique_ptr<PacketCache>>> _recordingRtpPacketCaches;
    std::unordered_map<size_t, std::unique_ptr<PacketCache>> _recordingEventPacketCache;

    std::mutex _configurationLock;

    RecordingStream* findRecordingStream(const std::string& recordingId);

    void stopTransportIfNeeded(transport::RtcTransport* streamTransport, const std::string& endpointId);
};

} // namespace bridge
