#pragma once

#include "memory/PacketPoolAllocator.h"
#include "transport/Endpoint.h"
#include "transport/EndpointFactory.h"
#include "transport/EndpointMetrics.h"
#include "transport/ice/IceSession.h"
#include <memory>

namespace bwe
{
struct Config;
struct RateControllerConfig;
} // namespace bwe

namespace config
{
class Config;
}

namespace jobmanager
{
class JobManager;
}

namespace sctp
{
struct SctpConfig;
}

namespace transport
{

class SrtpClientFactory;
class RecordingTransport;
class RtcePoll;
class RtcTransport;
class SocketAddress;
class ProbeServer;
typedef std::vector<std::shared_ptr<Endpoint>> Endpoints;

class TransportFactory
{
public:
    virtual ~TransportFactory() = default;
    virtual std::shared_ptr<RtcTransport> create(const ice::IceRole iceRole,
        const size_t endpointId,
        size_t expectedInboundStreamCount,
        size_t expectedOutboundStreamCount,
        size_t jobQueueSize,
        bool enableUplinkEstimation,
        bool enableDownlinkEstimation) = 0;
    virtual std::shared_ptr<RtcTransport> create(const ice::IceRole iceRole, const size_t endpointId) = 0;
    virtual std::shared_ptr<RtcTransport> create(const size_t endpointIdHash) = 0;
    virtual std::shared_ptr<RtcTransport> createOnSharedPort(const ice::IceRole iceRole,
        const size_t endpointIdHash) = 0;
    virtual std::shared_ptr<RtcTransport> createOnSharedPort(const ice::IceRole iceRole,
        const size_t endpointIdHash,
        size_t expectedInboundStreamCount,
        size_t expectedOutboundStreamCount,
        size_t jobQueueSize,
        bool enableUplinkEstimation,
        bool enableDownlinkEstimation) = 0;
    virtual std::shared_ptr<RtcTransport> createOnPrivatePort(const ice::IceRole iceRole,
        const size_t endpointIdHash) = 0;
    virtual std::shared_ptr<RtcTransport> createOnPrivatePort(const ice::IceRole iceRole,
        const size_t endpointIdHash,
        size_t expectedInboundStreamCount,
        size_t expectedOutboundStreamCount,
        size_t jobQueueSize,
        bool enableUplinkEstimation,
        bool enableDownlinkEstimation) = 0;
    virtual std::unique_ptr<RecordingTransport> createForRecording(const size_t endpointHashId,
        const size_t streamHashId,
        const SocketAddress& peer,
        const uint8_t aesKey[32],
        const uint8_t salt[12]) = 0;
    virtual EndpointMetrics getSharedUdpEndpointsMetrics() const = 0;
    virtual bool isGood() const = 0;

    virtual std::shared_ptr<RtcTransport> createOnPorts(const ice::IceRole iceRole,
        const size_t endpointIdHash,
        const Endpoints& rtpPorts,
        size_t expectedInboundStreamCount,
        size_t expectedOutboundStreamCount,
        size_t jobQueueSize,
        bool enableUplinkEstimation,
        bool enableDownlinkEstimation) = 0;

    virtual bool openRtpMuxPorts(Endpoints& rtpPorts, uint32_t maxSessions) const = 0;

    virtual void maintenance(uint64_t timestamp) = 0;

    virtual void registerIceListener(Endpoint::IEvents&, const std::string& ufrag) = 0;
    virtual void registerIceListener(ServerEndpoint::IEvents&, const std::string& ufrag) = 0;
    virtual void unregisterIceListener(Endpoint::IEvents&, const std::string& ufrag) = 0;
    virtual void unregisterIceListener(ServerEndpoint::IEvents&, const std::string& ufrag) = 0;
};

std::unique_ptr<TransportFactory> createTransportFactory(jobmanager::JobManager& jobManager,
    SrtpClientFactory& srtpClientFactory,
    const config::Config& config,
    const sctp::SctpConfig& sctpConfig,
    const ice::IceConfig& iceConfig,
    const bwe::Config& bweConfig,
    const bwe::RateControllerConfig& rateControllerConfig,
    const std::vector<SocketAddress>& interfaces,
    transport::RtcePoll& rtcePoll,
    memory::PacketPoolAllocator& mainAllocator,
    std::shared_ptr<EndpointFactory> endpointFactory);

} // namespace transport
