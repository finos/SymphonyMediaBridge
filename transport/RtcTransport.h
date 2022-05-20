#pragma once

#include "jobmanager/JobManager.h"
#include "memory/AudioPacketPoolAllocator.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/DataReceiver.h"
#include "transport/PacketCounters.h"
#include "transport/RtpReceiveState.h"
#include "transport/RtpSenderState.h"
#include "transport/Transport.h"
#include "transport/ice/IceSession.h"
#include "webrtc/DataStreamTransport.h"
#include <unordered_map>
#include <vector>

namespace config
{
class Config;
}

namespace bwe
{
struct Config;
struct RateControllerConfig;
} // namespace bwe
namespace sctp
{
struct SctpConfig;
}
namespace jobmanager
{
class JobQueue;
}

namespace transport
{

class SrtpClientFactory;
class Endpoint;
class ServerEndpoint;
class TcpEndpointFactory;
class UdpEndpoint;

class RtcTransport : public Transport, public webrtc::DataStreamTransport
{
public:
    virtual ~RtcTransport() = default;

    virtual void removeSrtpLocalSsrc(const uint32_t ssrc) = 0;
    virtual bool setSrtpRemoteRolloverCounter(const uint32_t ssrc, const uint32_t rolloverCounter) = 0;

    virtual bool isGatheringComplete() const = 0;
    virtual ice::IceCandidates getLocalCandidates() = 0;
    virtual std::pair<std::string, std::string> getLocalCredentials() = 0;

    virtual bool setRemotePeer(const SocketAddress& target) = 0;
    virtual const SocketAddress& getRemotePeer() const = 0;
    virtual void setRemoteIce(const std::pair<std::string, std::string>& credentials,
        const ice::IceCandidates& candidates,
        memory::AudioPacketPoolAllocator& allocator) = 0;
    virtual void setRemoteDtlsFingerprint(const std::string& fingerprintType,
        const std::string& fingerprintHash,
        const bool dtlsClientSide) = 0;
    virtual void disableDtls() = 0;
    virtual SocketAddress getLocalRtpPort() const = 0;
    virtual void setSctp(uint16_t localPort, uint16_t remotePort) = 0;
    virtual void connectSctp() = 0;

    virtual bool isDtlsClient() = 0;

    virtual void setAudioPayloadType(uint8_t payloadType, uint32_t rtpFrequency) = 0;
    virtual void setAbsSendTimeExtensionId(uint8_t extensionId) = 0;

    virtual bool isIceEnabled() const = 0;
    virtual bool isDtlsEnabled() const = 0;

    virtual uint32_t getSenderLossCount() const = 0;
    virtual uint32_t getUplinkEstimateKbps() const = 0;
    virtual uint32_t getDownlinkEstimateKbps() const = 0;
    virtual uint32_t getPacingQueueCount() const = 0;
    virtual uint32_t getRtxPacingQueueCount() const = 0;
    virtual uint64_t getRtt() const = 0;
    virtual PacketCounters getCumulativeReceiveCounters(uint32_t ssrc) const = 0;
    virtual PacketCounters getCumulativeAudioReceiveCounters() const = 0;
    virtual PacketCounters getCumulativeVideoReceiveCounters() const = 0;
    virtual PacketCounters getAudioReceiveCounters(uint64_t idleTimestamp) const = 0;
    virtual PacketCounters getVideoReceiveCounters(uint64_t idleTimestamp) const = 0;
    virtual PacketCounters getAudioSendCounters(uint64_t idleTimestamp) const = 0;
    virtual PacketCounters getVideoSendCounters(uint64_t idleTimestamp) const = 0;
    virtual void getReportSummary(std::unordered_map<uint32_t, ReportSummary>& outReportSummary) const = 0;

    virtual void setRtxProbeSource(uint32_t ssrc, uint32_t* sequenceCounter) = 0;

    virtual void runTick(uint64_t timestamp) = 0;
};

std::shared_ptr<RtcTransport> createTransport(jobmanager::JobManager& jobmanager,
    SrtpClientFactory& srtpClientFactory,
    const size_t endpointIdHash,
    const config::Config& config,
    const sctp::SctpConfig& sctpConfig,
    const bwe::Config& bweConfig,
    const bwe::RateControllerConfig& rateControllerConfig,
    const std::vector<Endpoint*>& rtpEndPoints,
    const std::vector<Endpoint*>& rtcpEndPoints,
    memory::PacketPoolAllocator& allocator);

std::shared_ptr<RtcTransport> createTransport(jobmanager::JobManager& jobmanager,
    SrtpClientFactory& srtpClientFactory,
    const size_t endpointIdHash,
    const config::Config& config,
    const sctp::SctpConfig& sctpConfig,
    const ice::IceConfig& iceConfig,
    ice::IceRole iceRole,
    const bwe::Config& bweConfig,
    const bwe::RateControllerConfig& rateControllerConfig,
    const std::vector<Endpoint*>& sharedEndPoints,
    const std::vector<ServerEndpoint*>& tcpEndpoints,
    TcpEndpointFactory* tcpEndpointFactory,
    memory::PacketPoolAllocator& allocator);

} // namespace transport
