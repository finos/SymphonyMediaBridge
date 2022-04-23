#pragma once

#include "memory/PacketPoolAllocator.h"
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

class TransportFactory
{
public:
    virtual ~TransportFactory() = default;
    virtual std::shared_ptr<RtcTransport> create(const ice::IceRole iceRole,
        const size_t sendPoolSize,
        const size_t endpointId) = 0;
    virtual std::shared_ptr<RtcTransport> create(const size_t sendPoolSize, const size_t endpointIdHash) = 0;
    virtual std::shared_ptr<RtcTransport> createOnSharedPort(const ice::IceRole iceRole,
        const size_t sendPoolSize,
        const size_t endpointIdHash) = 0;
    virtual std::shared_ptr<RtcTransport> createOnPrivatePort(const ice::IceRole iceRole,
        const size_t sendPoolSize,
        const size_t endpointIdHash) = 0;
    virtual std::unique_ptr<RecordingTransport> createForRecording(const size_t endpointHashId,
        const size_t streamHashId,
        const SocketAddress& peer,
        const uint8_t aesKey[32],
        const uint8_t salt[12]) = 0;
    virtual EndpointMetrics getSharedUdpEndpointsMetrics() const = 0;
    virtual bool isGood() const = 0;

    virtual void maintenance(uint64_t timestamp);
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
    memory::PacketPoolAllocator& mainAllocator);

} // namespace transport
