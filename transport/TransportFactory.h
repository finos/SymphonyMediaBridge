#pragma once

#include "memory/PacketPoolAllocator.h"
#include "transport/RecordingTransport.h"
#include "transport/RtcTransport.h"
#include <memory>

namespace jobmanager
{
class JobManager;
}

namespace ice
{
struct IceConfig;
}

namespace sctp
{
struct SctpConfig;
}

namespace transport
{

class SrtpClientFactory;
class RtcePoll;
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
    virtual bool isGood() const = 0;
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
