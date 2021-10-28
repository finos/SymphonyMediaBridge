#pragma once

#include "config/Config.h"
#include "crypto/AesGcmIvGenerator.h"
#include "crypto/SslHelper.h"
#include "jobmanager/JobManager.h"
#include "transport/DataReceiver.h"
#include "transport/RecordingEndpoint.h"
#include "transport/Transport.h"

namespace transport
{

class RtpSenderState;

class RecordingTransport final : public Transport, public RecordingEndpoint::IRecordingEvents
{
public:
    RecordingTransport(jobmanager::JobManager& jobManager,
        const config::Config& config,
        RecordingEndpoint* recordingEndpoint,
        const size_t endpointIdHash,
        const size_t streamIdHash,
        const SocketAddress& remotePeer,
        const uint8_t aesKey[32],
        const uint8_t salt[12]);

    virtual ~RecordingTransport() = default;

    bool isInitialized() const override { return _isInitialized; }
    const logger::LoggableId& getLoggableId() const override { return _loggableId; }

    size_t getId() const override { return _loggableId.getInstanceId(); }
    size_t getEndpointIdHash() const override { return _endpointIdHash; };

    void stop() override;
    bool isRunning() const override { return _isRunning && _isInitialized; }
    bool hasPendingJobs() const override { return _jobCounter.load() > 0; }
    std::atomic_uint32_t& getJobCounter() override { return _jobCounter; };
    void protectAndSend(memory::Packet* packet, memory::PacketPoolAllocator& sendAllocator) override;
    bool unprotect(memory::Packet* packet) override;
    void setDataReceiver(DataReceiver* dataReceiver) override;
    bool isConnected() override;
    bool start() override;
    void connect() override{};
    jobmanager::SerialJobManager& getJobQueue() override { return _serialJobManager; }

    void onRecControlReceived(RecordingEndpoint& endpoint,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::Packet* packet,
        memory::PacketPoolAllocator& allocator) override;

    void onUnregistered(RecordingEndpoint& endpoint) override;

    size_t getStreamIdHash() const { return _streamIdHash; }
    const SocketAddress& getRemotePeer() const { return _peerPort; }

private:
    friend class RecSendJob;

    struct RtcpMaintenance
    {
        RtcpMaintenance(uint64_t reportIntervalNs) : lastSendTime(0), reportInterval(reportIntervalNs) {}

        uint64_t lastSendTime;
        uint64_t reportInterval;
    };

    void doProtectAndSend(memory::Packet* packet, memory::PacketPoolAllocator& sendAllocator);
    uint32_t getRolloverCounter(uint32_t ssrc, uint16_t sequenceNumber);

    void sendRtcpSenderReport(memory::PacketPoolAllocator& sendAllocator, uint64_t timestamp);
    void onSendingStreamAddedEvent(memory::Packet* packet);
    void onSendingStreamRemovedEvent(memory::Packet* packet);
    RtpSenderState* getOutboundSsrc(const uint32_t ssrc);

    void protectAndSend(memory::Packet* packet,
        const SocketAddress& target,
        Endpoint* endpoint,
        memory::PacketPoolAllocator& allocator);

    std::atomic_bool _isInitialized;
    logger::LoggableId _loggableId;
    const config::Config& _config;

    size_t _endpointIdHash;
    size_t _streamIdHash;

    std::atomic_bool _isRunning;

    RecordingEndpoint* _recordingEndpoint;
    transport::SocketAddress _peerPort;

    std::atomic<DataReceiver*> _dataReceiver;

    std::atomic_uint32_t _jobCounter;
    jobmanager::SerialJobManager _serialJobManager;

    std::unique_ptr<crypto::AES> _aes;
    std::unique_ptr<crypto::AesGcmIvGenerator> _ivGenerator;

    concurrency::MpmcHashmap32<uint32_t, uint16_t> _previousSequenceNumber;
    concurrency::MpmcHashmap32<uint32_t, uint32_t> _rolloverCounter;
    concurrency::MpmcHashmap32<uint32_t, RtpSenderState> _outboundSsrcCounters;

    RtcpMaintenance _rtcp;
};

std::unique_ptr<RecordingTransport> createRecordingTransport(jobmanager::JobManager& jobManager,
    const config::Config& config,
    RecordingEndpoint* recordingEndpoint,
    const size_t endpointIdHash,
    const size_t streamIdHash,
    const SocketAddress& peer,
    const uint8_t aesKey[32],
    const uint8_t salt[12]);

} // namespace transport
