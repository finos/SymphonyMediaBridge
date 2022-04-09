#pragma once

#include "bridge/engine/EngineStats.h"
#include "bridge/engine/SimulcastStream.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "concurrency/MpmcHashmap.h"
#include "memory/AudioPacketPoolAllocator.h"
#include "memory/PacketPoolAllocator.h"
#include "memory/RingBuffer.h"
#include "transport/RtcTransport.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace config
{
class Config;
}

namespace memory
{
class Packet;
}

namespace jobmanager
{
class JobManager;
}

namespace bridge
{
class EngineStreamDirector;
class ActiveMediaList;
class PacketCache;
struct SsrcWhitelist;
struct RecordingDescription;
class EngineMessageListener;
class SsrcOutboundContext;
class UnackedPacketsTracker;
struct EngineAudioStream;
struct EngineDataStream;
struct EngineRecordingStream;
struct EngineVideoStream;
struct SimulcastLevel;

class EngineMixer : public transport::DataReceiver
{
public:
    // Internal EngineMixer sample rate, channels and sample size
    static constexpr size_t sampleRate = 48000;
    static constexpr size_t channelsPerFrame = 2;
    static constexpr size_t bytesPerSample = sizeof(int16_t);

    static constexpr size_t iterationDurationMs = 10;
    static constexpr size_t framesPerIteration48kHz = sampleRate / (1000 / iterationDurationMs);
    static constexpr size_t framesPerIteration1kHz = iterationDurationMs;
    static constexpr size_t samplesPerIteration = framesPerIteration48kHz * channelsPerFrame;
    static constexpr size_t preBufferSamples = samplesPerIteration * 50; // 500 ms
    static constexpr size_t minimumSamplesInBuffer = samplesPerIteration * 25; // 250 ms
    static constexpr size_t audioBufferSamples = preBufferSamples * 2; // 1000 ms
    static constexpr size_t ticksPerSSRCCheck = 100; // 1000 ms

    using AudioBuffer = memory::RingBuffer<int16_t, audioBufferSamples, preBufferSamples>;

    EngineMixer(const std::string& id,
        jobmanager::JobManager& jobManager,
        EngineMessageListener& messageListener,
        const size_t inactivityTimeoutMs,
        const uint32_t localVideoSsrc,
        const config::Config& config,
        memory::PacketPoolAllocator& sendAllocator,
        memory::AudioPacketPoolAllocator& audioAllocator,
        const std::vector<uint32_t>& audioSsrcs,
        const std::vector<SimulcastLevel>& videoSsrcs,
        const uint32_t lastN);
    ~EngineMixer() override;

    const std::string& getId() const { return _id; }
    const logger::LoggableId& getLoggableId() const { return _loggableId; }

    void addAudioStream(EngineAudioStream* engineAudioStream);
    void removeAudioStream(EngineAudioStream* engineAudioStream);
    void addAudioBuffer(const uint32_t ssrc, AudioBuffer* audioBuffer);
    void addVideoStream(EngineVideoStream* engineVideoStream);
    void removeVideoStream(EngineVideoStream* engineVideoStream);
    void addRecordingStream(EngineRecordingStream* engineRecordingStream);
    void removeRecordingStream(EngineRecordingStream* engineRecordingStream);
    void addDataSteam(EngineDataStream* engineDataStream);
    void removeDataStream(EngineDataStream* engineDataStream);
    void startTransport(transport::RtcTransport* transport);
    void startRecordingTransport(transport::RecordingTransport* transport);
    void reconfigureAudioStream(const transport::RtcTransport* transport, const uint32_t remoteSsrc);
    void reconfigureVideoStream(const transport::RtcTransport* transport,
        const SsrcWhitelist& ssrcWhitelist,
        const SimulcastStream& simulcastStream,
        const SimulcastStream* secondarySimulcastStream = nullptr);
    void addVideoPacketCache(const uint32_t ssrc, const size_t endpointIdHash, PacketCache* videoPacketCache);
    void handleSctpControl(const size_t endpointIdHash, memory::PacketPtr packet);
    void pinEndpoint(const size_t endpointIdHash, const size_t targetEndpointIdHash);
    void sendEndpointMessage(const size_t toEndpointIdHash, const size_t fromEndpointIdHash, const char* message);
    void recordingStart(EngineRecordingStream* stream, const RecordingDescription* desc);
    void recordingStop(EngineRecordingStream* stream, const RecordingDescription* desc);
    void updateRecordingStreamModalities(EngineRecordingStream* engineRecordingStream,
        bool isAudioEnabled,
        bool isVideoEnabled,
        bool isScreenSharingEnabled);
    void addRecordingRtpPacketCache(const uint32_t ssrc, const size_t endpointIdHash, PacketCache* packetCache);
    void addTransportToRecordingStream(const size_t streamIdHash,
        transport::RecordingTransport* transport,
        UnackedPacketsTracker* recUnackedPacketsTracker);
    void removeTransportFromRecordingStream(const size_t streamIdHash, const size_t endpointIdHash);

    void clear();

    memory::PacketPoolAllocator& getSendAllocator() { return _sendAllocator; }

    /**
     * Discard incoming packets in queues when engine no longer serves this mixer to ensure decrement of ref counts.
     */
    void flush();

    void run(const uint64_t engineIterationStartTimestamp);

    EngineStats::MixerStats gatherStats(const uint64_t engineIterationStartTimestamp);

    void onRtpPacketReceived(transport::RtcTransport* sender,
        memory::PacketPtr packet,
        uint32_t extendedSequenceNumber,
        uint64_t timestamp) override;

    void onConnected(transport::RtcTransport* sender) override;

    bool onSctpConnectionRequest(transport::RtcTransport* sender, uint16_t remotePort) override;
    void onSctpEstablished(transport::RtcTransport* sender) override;
    void onSctpMessage(transport::RtcTransport* sender,
        uint16_t streamId,
        uint16_t streamSequenceNumber,
        uint32_t payloadProtocol,
        const void* data,
        size_t length) override;

    void onRecControlReceived(transport::RecordingTransport* sender,
        memory::PacketPtr packet,
        uint64_t timestamp) override;

    void onForwarderAudioRtpPacketDecrypted(transport::RtcTransport* sender,
        memory::PacketPtr packet,
        const uint32_t extendedSequenceNumber);

    void onForwarderVideoRtpPacketDecrypted(transport::RtcTransport* sender,
        memory::PacketPtr packet,
        const uint32_t extendedSequenceNumber);

    void onMixerAudioRtpPacketDecoded(transport::RtcTransport* sender, memory::AudioPacketPtr packet);

    void onRtcpPacketDecoded(transport::RtcTransport* sender, memory::PacketPtr packet, uint64_t timestamp) override;

    jobmanager::JobManager& getJobManager() { return _jobManager; }

    // call only on related Transport serialjob context
    void tryRemoveInboundSsrc(uint32_t ssrc);

private:
    static const size_t maxPendingPackets = 4096;
    static const size_t maxPendingRtcpPackets = 2048;
    static const size_t maxSsrcs = 8192;
    static const size_t maxStreamsPerModality = 4096;
    static const size_t maxRecordingStreams = 8;

    template <typename PacketT, typename AllocatorT>
    struct IncomingPacketAggregate
    {
        IncomingPacketAggregate() : _transport(nullptr), _extendedSequenceNumber(0) {}

        IncomingPacketAggregate(PacketT packet, transport::RtcTransport* transport)
            : _packet(std::move(packet)),
              _transport(transport),
              _extendedSequenceNumber(0)
        {
        }

        IncomingPacketAggregate(PacketT packet,
            transport::RtcTransport* transport,
            const uint32_t extendedSequenceNumber)
            : _packet(std::move(packet)),
              _transport(transport),
              _extendedSequenceNumber(extendedSequenceNumber)
        {
        }

        PacketT _packet;
        transport::RtcTransport* _transport;
        uint32_t _extendedSequenceNumber;

        inline void lockOwner() const
        {
            if (_transport)
            {
                ++_transport->getJobCounter();
            }
        }

        inline void release() const
        {
            if (_transport)
            {
#if DEBUG
                const auto decreased = --_transport->getJobCounter();
                assert(decreased < 0xFFFFFFFF); // detecting going below zero
#else
                --_transport->getJobCounter();
#endif
            }
        }
    };

    using IncomingPacketInfo = IncomingPacketAggregate<memory::PacketPtr, memory::PacketPoolAllocator>;
    using IncomingAudioPacketInfo = IncomingPacketAggregate<memory::AudioPacketPtr, memory::AudioPacketPoolAllocator>;

    std::string _id;
    logger::LoggableId _loggableId;

    jobmanager::JobManager& _jobManager;
    EngineMessageListener& _messageListener;

    concurrency::MpmcHashmap32<uint32_t, AudioBuffer*> _mixerSsrcAudioBuffers;

    concurrency::MpmcQueue<IncomingPacketInfo> _incomingForwarderAudioRtp;
    concurrency::MpmcQueue<IncomingAudioPacketInfo> _incomingMixerAudioRtp;
    concurrency::MpmcQueue<IncomingPacketInfo> _incomingRtcp;
    concurrency::MpmcQueue<IncomingPacketInfo> _incomingForwarderVideoRtp;

    concurrency::MpmcHashmap32<size_t, EngineAudioStream*> _engineAudioStreams;
    concurrency::MpmcHashmap32<size_t, EngineVideoStream*> _engineVideoStreams;
    concurrency::MpmcHashmap32<size_t, EngineDataStream*> _engineDataStreams;
    concurrency::MpmcHashmap32<size_t, EngineRecordingStream*> _engineRecordingStreams;

    concurrency::MpmcHashmap32<uint32_t, SsrcInboundContext> _ssrcInboundContexts;

    uint32_t _localVideoSsrc;

    int16_t _mixedData[samplesPerIteration];
    uint64_t _rtpTimestampSource; // 1kHz. it works with wrapping since it is truncated to uint32.

    memory::PacketPoolAllocator& _sendAllocator;
    memory::AudioPacketPoolAllocator& _audioAllocator;

    size_t _noIncomingPacketsIntervalMs;
    const size_t _maxNoIncomingPacketsIntervalMs;

    size_t _noTicks;
    const size_t _ticksPerSSRCCheck;

    std::unique_ptr<EngineStreamDirector> _engineStreamDirector;
    std::unique_ptr<ActiveMediaList> _activeMediaList;
    uint64_t _lastUplinkEstimateUpdate;
    const config::Config& _config;
    uint32_t _lastN;
    uint32_t _numMixedAudioStreams;

    uint64_t _lastVideoBandwidthCheck;

    void processIncomingRtpPackets(const uint64_t timestamp);
    uint32_t processIncomingVideoRtpPackets(const uint64_t timestamp);
    void processIncomingRtcpPackets(const uint64_t timestamp);
    void processIncomingPayloadSpecificRtcpPacket(const size_t rtcpSenderEndpointIdHash,
        const rtp::RtcpHeader& rtcpPacket);
    void processIncomingTransportFbRtcpPacket(const transport::RtcTransport* transport,
        const rtp::RtcpHeader& rtcpPacket,
        const uint64_t timestamp);
    void checkVideoBandwidth(const uint64_t timestamp);
    void runTransportTicks(const uint64_t timestamp);

    void mixSsrcBuffers();
    void processAudioStreams();
    void runDominantSpeakerCheck(const uint64_t engineIterationStartTimestamp);
    void updateDirectorUplinkEstimates(const uint64_t engineIterationStartTimestamp);
    void processMissingPackets(const uint64_t timestamp);
    void checkPacketCounters(const uint64_t timestamp);
    void onVideoRtpPacketReceived(SsrcInboundContext* ssrcContext,
        transport::RtcTransport* sender,
        memory::PacketPtr packet,
        const uint32_t extendedSequenceNumber,
        const uint64_t timestamp);
    void onVideoRtpRtxPacketReceived(SsrcInboundContext* ssrcContext,
        transport::RtcTransport* sender,
        memory::PacketPtr packet,
        const uint32_t extendedSequenceNumber,
        const uint64_t timestamp);

    bool enqueuePacket(IncomingPacketInfo& packetInfo, concurrency::MpmcQueue<IncomingPacketInfo>& queue);

    SsrcInboundContext* getInboundSsrcContext(const uint32_t ssrc);
    SsrcInboundContext* emplaceInboundSsrcContext(const uint32_t ssrc,
        transport::RtcTransport* sender,
        const uint32_t payloadType,
        const uint64_t timestamp);
    SsrcOutboundContext* obtainOutboundSsrcContext(EngineAudioStream& audioStream, const uint32_t ssrc);
    SsrcOutboundContext* obtainOutboundSsrcContext(EngineVideoStream& videoStream, const uint32_t ssrc);
    SsrcOutboundContext* getOutboundSsrcContext(EngineVideoStream& videoStream, const uint32_t ssrc);
    SsrcOutboundContext* getOutboundSsrcContext(EngineRecordingStream& recordingStream, const uint32_t ssrc);

    void sendPliForUsedSsrcs(EngineVideoStream& videoStream);
    void sendLastNListMessage(const size_t endpointIdHash);
    void sendLastNListMessageToAll();
    void sendMessagesToNewDataStreams();
    void updateBandwidthFloor();
    void sendDominantSpeakerMessageToAll(const size_t dominantSpeaker);
    void sendUserMediaMapMessage(const size_t endpointIdHash);
    void sendUserMediaMapMessageToAll();
    void sendDominantSpeakerToRecordingStream(EngineRecordingStream& recordingStream,
        const size_t dominantSpeaker,
        const std::string& dominantSpeakerEndpoint);
    void sendDominantSpeakerToRecordingStream(EngineRecordingStream& recordingStream);

    void updateSimulcastLevelActiveState(EngineVideoStream& videoStream, const SimulcastStream& simulcastStream);
    void markAssociatedVideoOutboundContextsForDeletion(EngineVideoStream* senderVideoStream,
        const uint32_t ssrc,
        const uint32_t feedbackSsrc);
    void markInboundContextForDeletion(const uint32_t ssrc);

    void startRecordingAllCurrentStreams(EngineRecordingStream& recordingStream);

    void sendRecordingAudioStream(EngineRecordingStream& targetStream,
        const EngineAudioStream& audioStream,
        bool isAdded);
    void updateRecordingAudioStreams(EngineRecordingStream& targetStream, bool enabled);
    void sendRecordingVideoStream(EngineRecordingStream& targetStream,
        const EngineVideoStream& videoStream,
        SimulcastStream::VideoContentType contentType,
        bool isAdded);
    void updateRecordingVideoStreams(EngineRecordingStream& targetStream,
        SimulcastStream::VideoContentType contentType,
        bool enabled);
    void sendRecordingSimulcast(EngineRecordingStream& targetStream,
        const EngineVideoStream& videoStream,
        const SimulcastStream& simulcast,
        bool isAdded);

    void sendAudioStreamToRecording(const EngineAudioStream& audioStream, bool isAdded);
    void sendVideoStreamToRecording(const EngineVideoStream& videoStream, bool isAdded);
    void removeVideoSsrcFromRecording(const EngineVideoStream& videoStream, uint32_t ssrc);

    void allocateRecordingRtpPacketCacheIfNecessary(SsrcOutboundContext& ssrcOutboundContext,
        EngineRecordingStream& recordingStream);

    void processRecordingMissingPackets(const uint64_t timestamp);
};

} // namespace bridge
