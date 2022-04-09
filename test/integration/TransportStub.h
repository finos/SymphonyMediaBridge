#pragma once

#include "concurrency/ScopedSpinLocker.h"
#include "transport/PacketCounters.h"
#include "transport/RtpReceiveState.h"
#include "transport/RtpSenderState.h"
#include "transport/Transport.h"
#include "utils/SocketAddress.h"
#include <functional>
#include <gtest/gtest.h>

class TransportStub : public transport::Transport
{
private:
    logger::LoggableId _loggableId;
    size_t _endpointIdHash;
    std::atomic<transport::DataReceiver*> _dataReceiver = {nullptr};
    memory::PacketPoolAllocator _receiveAllocator;
    ::testing::internal::Notification _ready;
    std::atomic_flag _sentPacketsLock = ATOMIC_FLAG_INIT;
    std::vector<memory::Packet> _sentPackets;
    std::atomic_uint32_t _ownerRefCounter;
    std::function<void()> _stopHandler;
    jobmanager::JobQueue _jobQueue;
    transport::SocketAddress _remoteSocketAddr;

public:
    explicit TransportStub(jobmanager::JobManager& jobManager, const size_t endpointIdHash)
        : _loggableId("TransportStub"),
          _endpointIdHash(endpointIdHash),
          _receiveAllocator(memory::packetPoolSize, "TransportStub"),
          _jobQueue(jobManager)
    {
    }

    ~TransportStub() {}
    void waitReady() { _ready.WaitForNotification(); }

    size_t getSentPacketsCount()
    {
        concurrency::ScopedSpinLocker locker(_sentPacketsLock);
        return _sentPackets.size();
    }

    std::vector<memory::Packet> getSentPacketsAndReset()
    {
        std::vector<memory::Packet> result;
        {
            concurrency::ScopedSpinLocker locker(_sentPacketsLock);
            _sentPackets.swap(result);
        }
        return result;
    }

    void emulateIncomingPacket(const memory::Packet& packetData, uint32_t remoteSsrc)
    {
        if (!_ownerRefCounter)
        {
            return;
        }
        utils::ScopedIncrement scopedOwnerLock(_ownerRefCounter);
        transport::DataReceiver* const dataReceiver = _dataReceiver.load();
        if (!dataReceiver)
        {
            return;
        }
        auto packet = memory::makeUniquePacket(_receiveAllocator, packetData);
        auto hdr = reinterpret_cast<rtp::RtpHeader*>(packet->get());
        hdr->ssrc = remoteSsrc;

        dataReceiver->onRtpPacketReceived(this, std::move(packet), 0, utils::Time::getAbsoluteTime());
    }

    void emulateIncomingPackets(const std::vector<memory::Packet>& packetsData, uint32_t remoteSsrc)
    {
        for (const auto& packetData : packetsData)
        {
            emulateIncomingPacket(packetData, remoteSsrc);
        }
    }

    void setStopHandler(std::function<void()> value) { _stopHandler = value; }

public: // transport::Transport
    bool isInitialized() const override { return true; }
    const logger::LoggableId& getLoggableId() const override { return _loggableId; }
    size_t getEndpointIdHash() const override { return _endpointIdHash; }
    size_t getId() const override { return _loggableId.getInstanceId(); }

    void stop() override
    {
        if (_stopHandler)
        {
            _stopHandler();
        }
    }

    bool isRunning() const override { return true; }
    bool hasPendingJobs() const override { return false; }
    std::atomic_uint32_t& getJobCounter() override { return _ownerRefCounter; }

    void protectAndSend(memory::Packet* packet, memory::PacketPoolAllocator& sendAllocator) override
    {
        {
            concurrency::ScopedSpinLocker locker(_sentPacketsLock);
            _sentPackets.emplace_back(*packet);
        }
        sendAllocator.free(packet);
    }

    virtual bool unprotect(memory::Packet* packet) override { return true; }
    void removeSrtpLocalSsrc(const uint32_t ssrc) override {}
    bool setSrtpRemoteRolloverCounter(const uint32_t ssrc, const uint32_t rolloverCounter) override { return true; };

    bool isGatheringComplete() const override { return true; }
    ice::IceCandidates getLocalCandidates() override { return ice::IceCandidates(); }
    std::pair<std::string, std::string> getLocalCredentials() override { return std::make_pair("", ""); }

    bool setRemotePeer(const transport::SocketAddress& peer) override { return true; }
    const transport::SocketAddress& getRemotePeer() const override { return _remoteSocketAddr; }
    transport::SocketAddress getLocalRtpPort() const override { return transport::SocketAddress(); }
    void setRemoteIce(const std::pair<std::string, std::string>& credentials,
        const ice::IceCandidates& candidates,
        memory::PacketPoolAllocator& allocator) override
    {
        return true;
    }
    void setRemoteDtlsFingerprint(const std::string& fingerprintType,
        const std::string& fingerprintHash,
        const bool isDtlsClient) override
    {
    }

    void disableDtls() override {}

    void setSctp(uint16_t localPort, uint16_t remotePort) override {}

    bool isDtlsEnabled() const override { return false; }
    void setDataReceiver(transport::DataReceiver* dataReceiver) override
    {
        if (dataReceiver)
        {
            const bool wasSet = !!_dataReceiver.exchange(dataReceiver);
            ASSERT_TRUE(!wasSet);
        }
        else
        {
            _dataReceiver.store(nullptr);
        }
    }

    void allowIncomingRtp(bool enable) override{};

    bool isConnected() override { return true; }
    bool isDtlsClient() override { return true; }

    bool start() override { return true; }

    bool isIceEnabled() const override { return true; }

    void connect() override { _ready.Notify(); }

    jobmanager::JobQueue& getJobManager() override { return _jobQueue; }

    uint32_t getSenderLossCount() const override { return 0; }
    uint32_t getUplinkEstimateKbps() const override { return 250; }
    uint32_t getDownlinkEstimateKbps() const override { return 250; }
    void setAudioPayloadType(uint8_t payloadType, uint32_t rtpFrequency) override {}
    void setAbsSendTimeExtensionId(uint8_t extensionId) override {}
    void sendSctp(memory::Packet* packet, memory::PacketPoolAllocator& allocator) override {}
    uint16_t allocateOutboundSctpStream() override { return 0; }

    uint32_t getRtt() const override { return 5 * utils::Time::ms; }
    transport::PacketCounters getCumulativeReceiveCounters(uint32_t ssrc) const override
    {
        return transport::PacketCounters();
    }
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
};
