#pragma once

#include "mocks/TransportMock.h"
#include "transport/RtcTransport.h"
#include <gmock/gmock.h>

namespace test
{

class RtcTransportMock : public TransportMock<transport::RtcTransport>
{
    MOCK_METHOD(void, removeSrtpLocalSsrc, (const uint32_t ssrc), (override));
    MOCK_METHOD(bool, setSrtpRemoteRolloverCounter, (const uint32_t ssrc, const uint32_t rolloverCounter), (override));

    MOCK_METHOD(bool, isGatheringComplete, (), (const override));
    MOCK_METHOD(ice::IceCandidates, getLocalCandidates, (), (override));
    MOCK_METHOD((std::pair<std::string, std::string>), getLocalIceCredentials, (), (override));

    MOCK_METHOD(bool, setRemotePeer, (const transport::SocketAddress& target), (override));
    MOCK_METHOD(transport::SocketAddress&, getRemotePeer, (), (const override));
    MOCK_METHOD(void,
        setRemoteIce,
        ((const std::pair<std::string, std::string>& credentials),
            const ice::IceCandidates& candidates,
            memory::AudioPacketPoolAllocator& allocator),
        (override));

    MOCK_METHOD(void, addRemoteIceCandidate, (const ice::IceCandidate& candidate), (override));

    MOCK_METHOD(void,
        asyncSetRemoteDtlsFingerprint,
        (const std::string& fingerprintType, const std::string& fingerprintHash, const bool dtlsClientSide),
        (override));

    MOCK_METHOD(void, asyncDisableSrtp, (), (override));
    MOCK_METHOD(transport::SocketAddress, getLocalRtpPort, (), (const override));
    MOCK_METHOD(void, setSctp, (uint16_t localPort, uint16_t remotePort), (override));
    MOCK_METHOD(void, connectSctp, (), (override));

    MOCK_METHOD(bool, isDtlsClient, (), (override));

    MOCK_METHOD(void,
        setAudioPayloads,
        (uint8_t payloadType, utils::Optional<uint8_t> telephoneEventPayloadType, uint32_t rtpFrequency),
        (override));
    MOCK_METHOD(void, setAbsSendTimeExtensionId, (uint8_t extensionId), (override));

    MOCK_METHOD(bool, isIceEnabled, (), (const override));
    MOCK_METHOD(bool, isDtlsEnabled, (), (const override));

    MOCK_METHOD(uint32_t, getSenderLossCount, (), (const override));
    MOCK_METHOD(uint32_t, getUplinkEstimateKbps, (), (const override));
    MOCK_METHOD(uint32_t, getDownlinkEstimateKbps, (), (const override));
    MOCK_METHOD(uint32_t, getPacingQueueCount, (), (const override));
    MOCK_METHOD(uint32_t, getRtxPacingQueueCount, (), (const override));

    // nano seconds
    MOCK_METHOD(uint64_t, getRtt, (), (const override));
    MOCK_METHOD(transport::PacketCounters, getCumulativeReceiveCounters, (uint32_t ssrc), (const override));
    MOCK_METHOD(transport::PacketCounters, getCumulativeAudioReceiveCounters, (), (const override));
    MOCK_METHOD(transport::PacketCounters, getCumulativeVideoReceiveCounters, (), (const override));
    MOCK_METHOD(transport::PacketCounters, getAudioReceiveCounters, (uint64_t idleTimestamp), (const override));
    MOCK_METHOD(transport::PacketCounters, getVideoReceiveCounters, (uint64_t idleTimestamp), (const override));
    MOCK_METHOD(transport::PacketCounters, getAudioSendCounters, (uint64_t idleTimestamp), (const override));
    MOCK_METHOD(transport::PacketCounters, getVideoSendCounters, (uint64_t idleTimestamp), (const override));
    MOCK_METHOD(void,
        getReportSummary,
        ((std::unordered_map<uint32_t, transport::ReportSummary> & outReportSummary)),
        (const override));

    MOCK_METHOD(uint64_t, getInboundPacketCount, (), (const override));

    MOCK_METHOD(void,
        setRtxProbeSource,
        (const uint32_t ssrc, uint32_t* sequenceCounter, const uint16_t payloadType),
        (override));

    MOCK_METHOD(void, runTick, (uint64_t timestamp), (override));
    MOCK_METHOD(ice::IceSession::State, getIceState, (), (const override));
    MOCK_METHOD(transport::SrtpClient::State, getDtlsState, (), (const override));

    MOCK_METHOD(utils::Optional<ice::TransportType>, getSelectedTransportType, (), (const override));

    MOCK_METHOD(void, setTag, (const char* tag), (override));
    MOCK_METHOD(const char*, getTag, (), (const override));

    MOCK_METHOD(uint64_t, getLastReceivedPacketTimestamp, (), (const override));

    MOCK_METHOD(bool,
        sendSctp,
        (uint16_t streamId, uint32_t protocolId, const void* data, uint16_t length),
        (override));

    MOCK_METHOD(uint16_t, allocateOutboundSctpStream, (), (override));

    MOCK_METHOD(void, getSdesKeys, (std::vector<srtp::AesKey> & sdesKeys), (const override));
    MOCK_METHOD(void, asyncSetRemoteSdesKey, (const srtp::AesKey& key), (override));
};

} // namespace test
