#pragma once

#include "transport/RtcTransport.h"

class DummyRtcTransport : public transport::RtcTransport
{
public:
    DummyRtcTransport(jobmanager::JobQueue& jobQueue) : _loggableId(""), _endpointIdHash(1), _jobQueue(jobQueue) {}

    bool isInitialized() const override { return true; }
    const logger::LoggableId& getLoggableId() const override { return _loggableId; }
    size_t getEndpointIdHash() const override { return _endpointIdHash; }
    size_t getId() const override { return 0; }
    void stop() override {}
    bool isRunning() const override { return true; }
    bool hasPendingJobs() const override { return true; }
    std::atomic_uint32_t& getJobCounter() override { return _jobCounter; }
    void protectAndSend(memory::UniquePacket packet) override {}
    bool unprotect(memory::Packet& packet) override { return true; }
    void removeSrtpLocalSsrc(const uint32_t ssrc) override {}
    bool setSrtpRemoteRolloverCounter(const uint32_t ssrc, const uint32_t rolloverCounter) override { return true; }
    bool isGatheringComplete() const override { return true; }
    ice::IceCandidates getLocalCandidates() override { return ice::IceCandidates(); }
    std::pair<std::string, std::string> getLocalIceCredentials() override
    {
        return std::pair<std::string, std::string>();
    };
    bool setRemotePeer(const transport::SocketAddress& target) override { return true; }
    void setRemoteIce(const std::pair<std::string, std::string>& credentials,
        const ice::IceCandidates& candidates,
        memory::AudioPacketPoolAllocator&) override
    {
    }
    void addRemoteIceCandidate(const ice::IceCandidate& candidate) override {}

    void asyncSetRemoteDtlsFingerprint(const std::string& fingerprintType,
        const std::string& fingerprintHash,
        const bool dtlsClientSide) override
    {
    }
    void asyncDisableSrtp() override{};
    transport::SocketAddress getLocalRtpPort() const override { return transport::SocketAddress(); }
    void setSctp(uint16_t localPort, uint16_t remotePort) override {}
    void connectSctp() override {}
    void setDataReceiver(transport::DataReceiver* dataReceiver) override {}
    bool isConnected() override { return true; }
    bool isDtlsClient() override { return true; }
    void setAudioPayloads(uint8_t payloadType,
        utils::Optional<uint8_t> telephoneEventPayloadType,
        uint32_t rtpFrequency) override
    {
    }
    void setAbsSendTimeExtensionId(uint8_t extensionId) override {}
    bool start() override { return true; }
    bool isIceEnabled() const override { return true; }

    void connect() override {}
    void runTick(uint64_t timestamp) override {}
    jobmanager::JobQueue& getJobQueue() override { return _jobQueue; }
    uint32_t getPacingQueueCount() const override { return 0; }
    uint32_t getRtxPacingQueueCount() const override { return 0; }
    uint32_t getSenderLossCount() const override { return 0; }
    uint32_t getUplinkEstimateKbps() const override { return 0; }
    uint32_t getDownlinkEstimateKbps() const override { return 0; }
    uint64_t getRtt() const override { return 0; }
    transport::PacketCounters getCumulativeReceiveCounters(uint32_t ssrc) const override
    {
        return transport::PacketCounters();
    }

    uint64_t getInboundPacketCount() const override { return 0; }

    transport::PacketCounters getCumulativeVideoReceiveCounters() const override { return transport::PacketCounters(); }
    transport::PacketCounters getCumulativeAudioReceiveCounters() const override { return transport::PacketCounters(); }

    transport::PacketCounters getAudioReceiveCounters(uint64_t idleTimestamp) const override
    {
        return transport::PacketCounters();
    }
    transport::PacketCounters getVideoReceiveCounters(uint64_t idleTimestamp) const override
    {
        return transport::PacketCounters();
    }
    transport::PacketCounters getAudioSendCounters(uint64_t idleTimestamp) const override
    {
        return transport::PacketCounters();
    }
    transport::PacketCounters getVideoSendCounters(uint64_t idleTimestamp) const override
    {
        return transport::PacketCounters();
    }

    void getReportSummary(std::unordered_map<uint32_t, transport::ReportSummary>& outReportSummary) const override {}

    bool sendSctp(uint16_t streamId, uint32_t protocolId, const void* data, uint16_t length) override { return true; }
    uint16_t allocateOutboundSctpStream() override { return 0; }
    const transport::SocketAddress& getRemotePeer() const override { return _socketAddress; }

    void setRtxProbeSource(const uint32_t ssrc, uint32_t* sequenceCounter, const uint16_t payloadType) override {}

    ice::IceSession::State getIceState() const override { return ice::IceSession::State::CONNECTED; };

    transport::SrtpClient::State getDtlsState() const override { return transport::SrtpClient::State::CONNECTED; }
    utils::Optional<ice::TransportType> getSelectedTransportType() const override
    {
        return utils::Optional<ice::TransportType>();
    }

    void setTag(const char* tag) override{};
    const char* getTag() const override { return nullptr; };

    uint64_t getLastReceivedPacketTimestamp() const override { return 0; }
    void getSdesKeys(std::vector<srtp::AesKey>& sdesKeys) const override {}
    void asyncSetRemoteSdesKey(const srtp::AesKey& key) override {}

    logger::LoggableId _loggableId;
    size_t _endpointIdHash;
    jobmanager::JobQueue& _jobQueue;
    std::atomic_uint32_t _jobCounter;

private:
    transport::SocketAddress _socketAddress = transport::SocketAddress();
};
