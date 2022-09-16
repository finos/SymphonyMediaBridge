#pragma once

#include "api/SimulcastGroup.h"
#include "bridge/engine/ActiveTalker.h"
#include "bridge/engine/BarbellEndpointMap.h"
#include "bridge/engine/EngineStats.h"
#include "bridge/engine/SimulcastStream.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "concurrency/MpmcHashmap.h"
#include "memory/AudioPacketPoolAllocator.h"
#include "memory/Map.h"
#include "memory/PacketPoolAllocator.h"
#include "memory/RingBuffer.h"
#include "transport/RtcTransport.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace config
{
class Config;
}

namespace memory
{
class Packet;
}

namespace webrtc
{
struct SctpStreamMessageHeader;
}

namespace rtp
{
struct RtcpFeedback;
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
struct EngineBarbell;

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

    static constexpr size_t maxNumBarbells = 16;

    using AudioBuffer = memory::RingBuffer<int16_t, audioBufferSamples, preBufferSamples>;

    EngineMixer(const std::string& id,
        jobmanager::JobManager& jobManager,
        EngineMessageListener& messageListener,
        const uint32_t localVideoSsrc,
        const config::Config& config,
        memory::PacketPoolAllocator& sendAllocator,
        memory::AudioPacketPoolAllocator& audioAllocator,
        const std::vector<uint32_t>& audioSsrcs,
        const std::vector<api::SimulcastGroup>& videoSsrcs,
        const uint32_t lastN);
    ~EngineMixer() override;

    const std::string& getId() const { return _id; }
    const logger::LoggableId& getLoggableId() const { return _loggableId; }

    // -- methods executed on engine thread
    void addAudioStream(EngineAudioStream* engineAudioStream);
    void removeStream(EngineAudioStream* engineAudioStream);
    void addAudioBuffer(const uint32_t ssrc, AudioBuffer* audioBuffer);
    void addVideoStream(EngineVideoStream* engineVideoStream);
    void removeStream(EngineVideoStream* engineVideoStream);
    void addRecordingStream(EngineRecordingStream* engineRecordingStream);
    void removeRecordingStream(EngineRecordingStream* engineRecordingStream);
    void addDataSteam(EngineDataStream* engineDataStream);
    void removeStream(EngineDataStream* engineDataStream);
    void startTransport(transport::RtcTransport* transport);
    void startRecordingTransport(transport::RecordingTransport* transport);
    void reconfigureAudioStream(const transport::RtcTransport* transport, const uint32_t remoteSsrc);
    void reconfigureVideoStream(const transport::RtcTransport* transport,
        const SsrcWhitelist& ssrcWhitelist,
        const SimulcastStream& simulcastStream,
        const SimulcastStream* secondarySimulcastStream = nullptr);
    void addVideoPacketCache(const uint32_t ssrc, const size_t endpointIdHash, PacketCache* videoPacketCache);
    void handleSctpControl(const size_t endpointIdHash, const memory::Packet& packet);
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
    void addBarbell(EngineBarbell* barbell);
    void removeBarbell(size_t idHash);
    void forwardPackets(const uint64_t engineTimestamp);
    void clear();
    EngineStats::MixerStats gatherStats(const uint64_t engineIterationStartTimestamp);

    void run(const uint64_t engineIterationStartTimestamp);
    // --

    memory::AudioPacketPoolAllocator& getAudioAllocator() { return _audioAllocator; }
    size_t getDominantSpeakerId() const;
    std::map<size_t, ActiveTalker> getActiveTalkers() const;
    utils::Optional<uint32_t> getUserId(const size_t ssrc) const;
    void mapSsrc2UserId(uint32_t ssrc, uint32_t usid);

    /**
     * Discard incoming packets in queues when engine no longer serves this mixer to ensure decrement of ref counts.
     */
    void flush();

    // -- executed on Transport thread context
    void onRtpPacketReceived(transport::RtcTransport* sender,
        memory::UniquePacket packet,
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
        memory::UniquePacket packet,
        uint64_t timestamp) override;
    void onForwarderAudioRtpPacketDecrypted(SsrcInboundContext& inboundContext,
        memory::UniquePacket packet,
        const uint32_t extendedSequenceNumber);
    void onForwarderVideoRtpPacketDecrypted(SsrcInboundContext& inboundContext,
        memory::UniquePacket packet,
        const uint32_t extendedSequenceNumber);
    void onMixerAudioRtpPacketDecoded(SsrcInboundContext& inboundContext, memory::UniqueAudioPacket packet);
    void onRtcpPacketDecoded(transport::RtcTransport* sender, memory::UniquePacket packet, uint64_t timestamp) override;
    void internalRemoveBarbell(size_t idHash);
    void internalRemoveInboundSsrc(uint32_t ssrc);
    // --

    jobmanager::JobManager& getJobManager() { return _jobManager; }

    // call only on related Transport thread context

private:
    static const size_t maxPendingPackets = 4096;
    static const size_t maxPendingRtcpPackets = 2048;
    static const size_t maxSsrcs = 8192;
    static const size_t maxStreamsPerModality = 4096;
    static const size_t maxRecordingStreams = 8;

    template <typename PacketT>
    class IncomingPacketAggregate
    {
    public:
        IncomingPacketAggregate() : _inboundContext(nullptr), _transport(nullptr), _extendedSequenceNumber(0) {}

        IncomingPacketAggregate(PacketT packet, transport::RtcTransport* transport)
            : _packet(std::move(packet)),
              _inboundContext(nullptr),
              _transport(transport),
              _extendedSequenceNumber(0)
        {
            assert(_packet);
            lockOwner();
        }

        IncomingPacketAggregate(PacketT packet, SsrcInboundContext* inboundContext)
            : _packet(std::move(packet)),
              _inboundContext(inboundContext),
              _transport(inboundContext->sender),
              _extendedSequenceNumber(0)
        {
            assert(_packet);
            lockOwner();
        }

        IncomingPacketAggregate(PacketT packet,
            transport::RtcTransport* transport,
            const uint32_t extendedSequenceNumber)
            : _packet(std::move(packet)),
              _inboundContext(nullptr),
              _transport(transport),
              _extendedSequenceNumber(extendedSequenceNumber)
        {
            assert(_packet);
            lockOwner();
        }

        IncomingPacketAggregate(PacketT packet,
            SsrcInboundContext* inboundContext,
            const uint32_t extendedSequenceNumber)
            : _packet(std::move(packet)),
              _inboundContext(inboundContext),
              _transport(inboundContext->sender),
              _extendedSequenceNumber(extendedSequenceNumber)
        {
            assert(_packet);
            lockOwner();
        }

        explicit IncomingPacketAggregate(IncomingPacketAggregate&& rhs)
            : _packet(std::move(rhs._packet)),
              _inboundContext(std::exchange(rhs._inboundContext, nullptr)),
              _transport(std::exchange(rhs._transport, nullptr)),
              _extendedSequenceNumber(rhs._extendedSequenceNumber)
        {
        }

        IncomingPacketAggregate& operator=(IncomingPacketAggregate&& rhs)
        {
            release();

            _packet = std::move(rhs._packet);
            _inboundContext = std::exchange(rhs._inboundContext, nullptr);
            _transport = std::exchange(rhs._transport, nullptr);
            _extendedSequenceNumber = rhs._extendedSequenceNumber;
            return *this;
        }

        IncomingPacketAggregate(const IncomingPacketAggregate&) = delete;
        IncomingPacketAggregate& operator=(const IncomingPacketAggregate&) = delete;

        ~IncomingPacketAggregate() { release(); }

        inline SsrcInboundContext* inboundContext() { return _inboundContext; }
        inline transport::RtcTransport* transport() { return _transport; }
        inline PacketT& packet() { return _packet; }
        inline const PacketT& packet() const { return _packet; }
        inline uint32_t extendedSequenceNumber() const { return _extendedSequenceNumber; }

    private:
        void release()
        {
            if (_transport)
            {
#if DEBUG
                const auto decreased = --_transport->getJobCounter();
                assert(decreased < 0xFFFFFFFF); // detecting going below zero
#else
                --_transport->getJobCounter();
#endif
                _transport = nullptr;
            }
        }

        void lockOwner() const
        {
            if (_transport)
            {
                ++_transport->getJobCounter();
            }
        }

        PacketT _packet;
        SsrcInboundContext* _inboundContext;
        transport::RtcTransport* _transport;
        uint32_t _extendedSequenceNumber;
    };

    using IncomingPacketInfo = IncomingPacketAggregate<memory::UniquePacket>;
    using IncomingAudioPacketInfo = IncomingPacketAggregate<memory::UniqueAudioPacket>;

    std::string _id;
    logger::LoggableId _loggableId;

    jobmanager::JobManager& _jobManager;
    EngineMessageListener& _messageListener;

    concurrency::MpmcHashmap32<uint32_t, AudioBuffer*> _mixerSsrcAudioBuffers;

    concurrency::MpmcQueue<IncomingPacketInfo> _incomingBarbellSctp;
    concurrency::MpmcQueue<IncomingPacketInfo> _incomingForwarderAudioRtp;
    concurrency::MpmcQueue<IncomingAudioPacketInfo> _incomingMixerAudioRtp;
    concurrency::MpmcQueue<IncomingPacketInfo> _incomingRtcp;
    concurrency::MpmcQueue<IncomingPacketInfo> _incomingForwarderVideoRtp;

    concurrency::MpmcHashmap32<size_t, EngineAudioStream*> _engineAudioStreams;
    concurrency::MpmcHashmap32<size_t, EngineVideoStream*> _engineVideoStreams;
    concurrency::MpmcHashmap32<size_t, EngineDataStream*> _engineDataStreams;
    concurrency::MpmcHashmap32<size_t, EngineRecordingStream*> _engineRecordingStreams;
    concurrency::MpmcHashmap32<size_t, EngineBarbell*> _engineBarbells;

    // active contexts
    concurrency::MpmcHashmap32<uint32_t, SsrcInboundContext*> _ssrcInboundContexts;
    // active and decommissioned contexts
    concurrency::MpmcHashmap32<uint32_t, SsrcInboundContext> _allSsrcInboundContexts;
    concurrency::MpmcHashmap32<uint32_t, uint32_t> _audioSsrcToUserIdMap;

    uint32_t _localVideoSsrc;

    int16_t _mixedData[samplesPerIteration];
    uint64_t _rtpTimestampSource; // 1kHz. it works with wrapping since it is truncated to uint32.

    memory::PacketPoolAllocator& _sendAllocator;
    memory::AudioPacketPoolAllocator& _audioAllocator;

    uint64_t _lastReceiveTime;

    uint64_t _lastCounterCheck;

    std::unique_ptr<EngineStreamDirector> _engineStreamDirector;
    std::unique_ptr<ActiveMediaList> _activeMediaList;
    uint64_t _lastUplinkEstimateUpdate;
    uint64_t _lastIdleTransportCheck;
    const config::Config& _config;
    uint32_t _lastN;
    uint32_t _numMixedAudioStreams;

    uint64_t _lastVideoBandwidthCheck;
    uint64_t _lastVideoPacketProcessed;
    bool _probingVideoStreams;
    uint32_t _minUplinkEstimate;

    uint32_t getMinRemoteClientDownlinkBandwidth() const;
    void reportMinRemoteClientDownlinkBandwidthToBarbells(const uint32_t minUplinkEstimate) const;

    //  -- methods executed on engine thread
    bool needToUpdateMinUplinkEstimate(const uint32_t curEstimate, const uint32_t oldEstimate) const;

    void processBarbellSctp(const uint64_t timestamp);
    void processIncomingRtpPackets(const uint64_t timestamp);
    void forwardVideoRtpPacket(IncomingPacketInfo& packetInfo, const uint64_t timestamp);
    void forwardVideoRtpPacketRecording(IncomingPacketInfo& packetInfo, const uint64_t timestamp);
    void forwardVideoRtpPacketOverBarbell(IncomingPacketInfo& packetInfo, const uint64_t timestamp);
    void forwardAudioRtpPacket(IncomingPacketInfo& packetInfo, uint64_t timestamp);
    void forwardAudioRtpPacketOverBarbell(IncomingPacketInfo& packetInfo, uint64_t timestamp);
    void forwardAudioRtpPacketRecording(IncomingPacketInfo& packetInfo, uint64_t timestamp);
    void addPacketToMixerBuffers(const IncomingAudioPacketInfo& packet, const uint64_t timestamp, bool logSpamGuard);

    void processIncomingRtcpPackets(const uint64_t timestamp);
    void processIncomingPayloadSpecificRtcpPacket(const size_t rtcpSenderEndpointIdHash,
        const rtp::RtcpHeader& rtcpPacket,
        uint64_t timestamp);
    void processIncomingBarbellFbRtcpPacket(EngineBarbell& barbell,
        const rtp::RtcpFeedback& rtcpFeedback,
        const uint64_t timestamp);
    void processIncomingTransportFbRtcpPacket(const transport::RtcTransport* transport,
        const rtp::RtcpHeader& rtcpPacket,
        const uint64_t timestamp);
    void checkVideoBandwidth(const uint64_t timestamp);
    void removeIdleStreams(const uint64_t timestamp);

    void mixSsrcBuffers();
    void processAudioStreams();
    void runDominantSpeakerCheck(const uint64_t engineIterationStartTimestamp);
    void updateDirectorUplinkEstimates(const uint64_t engineIterationStartTimestamp);
    void processMissingPackets(const uint64_t timestamp);
    void checkPacketCounters(const uint64_t timestamp);
    void checkIfRateControlIsNeeded(const uint64_t timestamp);
    bool isVideoInUse(const uint64_t timestamp, const uint64_t threshold) const;
    void markSsrcsInUse();

    void onPliRequestFromReceiver(size_t endpointIdHash, uint32_t ssrc, uint64_t timestamp);
    void sendLastNListMessage(const size_t endpointIdHash);
    void sendLastNListMessageToAll();
    void sendMessagesToNewDataStreams();
    void updateBandwidthFloor();
    void sendDominantSpeakerMessageToAll(const size_t dominantSpeaker);
    void sendUserMediaMapMessage(const size_t endpointIdHash);
    void sendUserMediaMapMessageToAll();
    void sendUserMediaMapMessageOverBarbells();
    void sendDominantSpeakerToRecordingStream(EngineRecordingStream& recordingStream,
        const size_t dominantSpeaker,
        const std::string& dominantSpeakerEndpoint);
    void sendDominantSpeakerToRecordingStream(EngineRecordingStream& recordingStream);

    void updateSimulcastLevelActiveState(EngineVideoStream& videoStream, const SimulcastStream& simulcastStream);
    void markAssociatedVideoOutboundContextsForDeletion(EngineVideoStream* senderVideoStream,
        const uint32_t ssrc,
        const uint32_t feedbackSsrc);
    void decommissionInboundContext(const uint32_t ssrc);

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

    void processEngineMissingPackets(bridge::SsrcInboundContext& ssrcInboundContext);
    void processBarbellMissingPackets(bridge::SsrcInboundContext& ssrcInboundContext);
    void processRecordingMissingPackets(const uint64_t timestamp);
    void startProbingVideoStream(EngineVideoStream&);
    void stopProbingVideoStream(const EngineVideoStream&);

    void onBarbellUserMediaMap(size_t barbellIdHash, const char* message);
    void onBarbellMinUplinkEstimate(size_t barbellIdHash, const char* message);
    void onBarbellDataChannelEstablish(size_t barbellIdHash,
        webrtc::SctpStreamMessageHeader& header,
        size_t packetSize);

    ////

    // -- methods executed on Transport thread context
    void onVideoRtpPacketReceived(SsrcInboundContext& ssrcContext,
        transport::RtcTransport* sender,
        memory::UniquePacket packet,
        const uint32_t extendedSequenceNumber,
        const uint64_t timestamp);
    void onVideoRtpRtxPacketReceived(SsrcInboundContext& ssrcContext,
        transport::RtcTransport* sender,
        memory::UniquePacket packet,
        const uint32_t extendedSequenceNumber,
        const uint64_t timestamp);

    SsrcInboundContext* emplaceInboundSsrcContext(const uint32_t ssrc,
        transport::RtcTransport* sender,
        const uint32_t payloadType,
        const uint64_t timestamp);

    SsrcOutboundContext* obtainOutboundSsrcContext(size_t endpointIdHash,
        concurrency::MpmcHashmap32<uint32_t, SsrcOutboundContext>& ssrcOutboundContexts,
        const uint32_t ssrc,
        const RtpMap& rtpMap);

    bool setPacketSourceEndpointIdHash(memory::Packet& packet, size_t barbellIdHash, uint32_t ssrc, bool isAudio);
    utils::Optional<uint32_t> findMainSsrc(size_t barbellIdHash, uint32_t feedbackSsrc);
    // --
};

} // namespace bridge
