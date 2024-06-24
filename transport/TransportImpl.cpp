#include "TransportImpl.h"
#include "api/utils.h"
#include "bwe/BandwidthEstimator.h"
#include "config/Config.h"
#include "dtls/SrtpClient.h"
#include "dtls/SrtpClientFactory.h"
#include "dtls/SslDtls.h"
#include "ice/IceSerialize.h"
#include "ice/IceSession.h"
#include "logger/Logger.h"
#include "logger/PacketLogger.h"
#include "memory/AudioPacketPoolAllocator.h"
#include "rtp/RtcpFeedback.h"
#include "rtp/RtpHeader.h"
#include "sctp/SctpAssociation.h"
#include "transport/DtlsJob.h"
#include "transport/IceJob.h"
#include "transport/SctpJob.h"
#include "transport/ice/IceSerialize.h"
#include "utils/Function.h"
#include "utils/SocketAddress.h"
#include "utils/StdExtensions.h"
#include <arpa/inet.h>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#define DEBUG_RTP 0

#if DEBUG_RTP
#define RTP_LOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
#define RTP_LOG(fmt, ...)
#endif
namespace transport
{
constexpr uint32_t Mbps100 = 100000;
// we have to serialize operations on srtp client
// timers, start and receive must be done from same serialized jobmanager.

struct IceCredentials
{
    char ufrag[64];
    char password[128];
};
static_assert(sizeof(IceCredentials) < memory::Packet::size, "IceCredentials does not fit into a Packet");

struct IceCandidates
{
    static const size_t maxCandidateCount = 10;
    size_t candidateCount = 0;
    ice::IceCandidate candidates[maxCandidateCount];
};
static_assert(sizeof(IceCandidates) < memory::Packet::size, "IceCandidates does not fit into a Packet");

class IceSetRemoteJob : public jobmanager::CountedJob
{
    static const size_t maxCandidatePackets = 4;

public:
    IceSetRemoteJob(TransportImpl& transport,
        const std::pair<std::string, std::string>& credentials,
        const ice::IceCandidates& candidates,
        memory::AudioPacketPoolAllocator& allocator)
        : CountedJob(transport.getJobCounter()),
          _transport(transport),
          _iceCredentials(memory::makeUniquePacket(allocator)),
          _iceCandidates(memory::makeUniquePacket(allocator))
    {
        if (!_iceCredentials || !_iceCandidates)
        {
            logger::error("failed to allocate packet", "setRemoteIce");
            _iceCredentials.reset();
            _iceCandidates.reset();
            return;
        }
        auto& iceSettings = *reinterpret_cast<IceCredentials*>(_iceCredentials->get());
        utils::strncpy(iceSettings.ufrag, credentials.first.c_str(), sizeof(iceSettings.ufrag));
        utils::strncpy(iceSettings.password, credentials.second.c_str(), sizeof(iceSettings.password));

        memory::MemoryFile stream(_iceCandidates->get(), _iceCandidates->size);
        for (auto& candidate : candidates)
        {
            stream << candidate;
            if (!stream.isGood())
            {
                logger::warn("Not all candidates could be added. ICE will likely work anyway.",
                    _transport.getLoggableId().c_str());
                break;
            }
        }
        _iceCandidates->setLength(stream.getPosition());
    }

    void run() override
    {
        if (_iceCredentials && _iceCandidates)
        {
            _transport.doSetRemoteIce(*_iceCredentials, *_iceCandidates);
        }
    }

private:
    TransportImpl& _transport;
    memory::UniqueAudioPacket _iceCredentials;
    memory::UniqueAudioPacket _iceCandidates;
};

struct DtlsCredentials
{
    char fingerprintHash[256];
    char fingerprintType[32];
    bool dtlsClientSide;
};
static_assert(sizeof(DtlsCredentials) < sizeof(memory::Packet) - sizeof(size_t),
    "DtlsCredentials does not fit into a Packet");

class DtlsSetRemoteJob : public jobmanager::CountedJob
{
public:
    DtlsSetRemoteJob(TransportImpl& transport,
        SrtpClient& srtpClient,
        const char* fingerprintHash,
        const char* fingerprintType,
        bool clientSide,
        memory::PacketPoolAllocator& allocator)
        : CountedJob(transport.getJobCounter()),
          _transport(transport),
          _srtpClient(srtpClient),
          _packet(nullptr)
    {
        DtlsCredentials credentials;
        utils::strncpy(credentials.fingerprintHash, fingerprintHash, sizeof(credentials.fingerprintHash));
        utils::strncpy(credentials.fingerprintType, fingerprintType, sizeof(credentials.fingerprintType));
        credentials.dtlsClientSide = clientSide;

        _packet = memory::makeUniquePacket(allocator, &credentials, sizeof(credentials));
    }

    void run() override
    {
        if (_packet && _srtpClient.getState() == SrtpClient::State::IDLE)
        {
            auto credentials = reinterpret_cast<DtlsCredentials*>(_packet->get());
            logger::info("set remote DTLS fingerprint %s, srtp %s, mode %s, %zu",
                _transport.getLoggableId().c_str(),
                _srtpClient.getLoggableId().c_str(),
                credentials->fingerprintType,
                credentials->dtlsClientSide ? "client" : "server",
                std::strlen(credentials->fingerprintHash));

            _srtpClient.setRemoteDtlsFingerprint(credentials->fingerprintType,
                credentials->fingerprintHash,
                credentials->dtlsClientSide);
        }
    }

private:
    TransportImpl& _transport;
    SrtpClient& _srtpClient;
    memory::UniquePacket _packet;
};

class SctpSendJob : public jobmanager::CountedJob
{
    struct SctpDataChunk
    {
        uint32_t payloadProtocol;
        uint16_t id;

        void* data() { return &id + 1; }
        const void* data() const { return &id + 1; };
    };

public:
    SctpSendJob(sctp::SctpAssociation& association,
        uint16_t streamId,
        uint32_t protocolId,
        const void* data,
        uint16_t length,
        memory::PacketPoolAllocator& allocator,
        jobmanager::JobQueue& jobQueue,
        TransportImpl& transport)
        : CountedJob(transport.getJobCounter()),
          _jobQueue(jobQueue),
          _sctpAssociation(association),
          _packet(memory::makeUniquePacket(allocator)),
          _transport(transport)
    {
        if (_packet)
        {
            if (sizeof(SctpDataChunk) + length > memory::Packet::size)
            {
                logger::error("sctp message too big %u", _transport.getLoggableId().c_str(), length);
                return;
            }

            auto* header = reinterpret_cast<SctpDataChunk*>(_packet->get());
            header->id = streamId;
            header->payloadProtocol = protocolId;
            std::memcpy(header->data(), data, length);
            _packet->setLength(sizeof(SctpDataChunk) + length);
        }
        else
        {
            logger::error("failed to create packet for outbound sctp", transport.getLoggableId().c_str());
        }
    }

    void run() override
    {
        if (!_packet)
        {
            return;
        }

        auto timestamp = utils::Time::getAbsoluteTime();
        auto currentTimeout = _sctpAssociation.nextTimeout(timestamp);
        auto& header = *reinterpret_cast<SctpDataChunk*>(_packet->get());
        if (!_sctpAssociation.sendMessage(header.id,
                header.payloadProtocol,
                header.data(),
                _packet->getLength() - sizeof(header),
                timestamp))
        {
            if (_transport.isConnected())
            {
                logger::error("failed to send SCTP message. sctp state %s. sctp streams %zu, queued %zuB",
                    _transport.getLoggableId().c_str(),
                    toString(_sctpAssociation.getState()),
                    _sctpAssociation.getStreamCount(),
                    _sctpAssociation.outboundPendingSize());
                return; // do not retry
            }
            else
            {
                logger::info("SCTP message sent too soon. sctp state %s, %zuB",
                    _transport.getLoggableId().c_str(),
                    toString(_sctpAssociation.getState()),
                    _packet->getLength());
            }
        }

        int64_t nextTimeout = _sctpAssociation.processTimeout(timestamp);
        while (nextTimeout == 0)
        {
            nextTimeout = _sctpAssociation.processTimeout(timestamp);
        }
        if (nextTimeout >= 0 && (currentTimeout < 0 || currentTimeout > nextTimeout))
        {
            SctpTimerJob::start(_jobQueue, _transport, _sctpAssociation, nextTimeout);
        }
    }

private:
    jobmanager::JobQueue& _jobQueue;
    sctp::SctpAssociation& _sctpAssociation;
    memory::UniquePacket _packet;
    TransportImpl& _transport;
};

// Placed last in queue during shutdown to reduce ref count when all jobs are complete.
// This means other jobs in the transport job queue do not have to have counters
class UnregisteredJob : public jobmanager::CountedJob
{
public:
    explicit UnregisteredJob(std::atomic_uint32_t& counter) : CountedJob(counter), _counter(counter) {}

    void run() override
    {
        auto value = --_counter;
        assert(value != 0xFFFFFFFFu);
    }

private:
    std::atomic_uint32_t& _counter;
};

class RunTickJob : public jobmanager::CountedJob
{
public:
    RunTickJob(TransportImpl& transport, const uint64_t timestamp)
        : CountedJob(transport.getJobCounter()),
          _transport(transport),
          _timestamp(timestamp)
    {
    }

    void run() override
    {
        if (utils::Time::diffLT(_transport._lastTickJobStartTimestamp, _timestamp, utils::Time::ms * 10))
        {
            return;
        }
        _transport.doRunTick(utils::Time::getAbsoluteTime());
    }

private:
    TransportImpl& _transport;
    uint64_t _timestamp;
};

std::shared_ptr<RtcTransport> createTransport(jobmanager::JobManager& jobmanager,
    SrtpClientFactory& srtpClientFactory,
    const size_t endpointIdHash,
    const config::Config& config,
    const sctp::SctpConfig& sctpConfig,
    const ice::IceConfig& iceConfig,
    ice::IceRole iceRole,
    const bwe::Config& bweConfig,
    const bwe::RateControllerConfig& rateControllerConfig,
    const Endpoints& rtpEndPoints,
    const ServerEndpoints& tcpEndpoints,
    TcpEndpointFactory* tcpEndpointFactory,
    memory::PacketPoolAllocator& allocator,
    size_t expectedInboundStreamCount,
    size_t expectedOutboundStreamCount,
    bool enableUplinkEstimation,
    bool enableDownlinkEstimation)
{
    return std::make_shared<TransportImpl>(jobmanager,
        srtpClientFactory,
        endpointIdHash,
        config,
        sctpConfig,
        iceConfig,
        iceRole,
        bweConfig,
        rateControllerConfig,
        rtpEndPoints,
        tcpEndpoints,
        tcpEndpointFactory,
        allocator,
        expectedInboundStreamCount,
        expectedOutboundStreamCount,
        enableUplinkEstimation,
        enableDownlinkEstimation);
}

std::shared_ptr<RtcTransport> createTransport(jobmanager::JobManager& jobmanager,
    SrtpClientFactory& srtpClientFactory,
    const size_t endpointIdHash,
    const config::Config& config,
    const sctp::SctpConfig& sctpConfig,
    const bwe::Config& bweConfig,
    const bwe::RateControllerConfig& rateControllerConfig,
    const Endpoints& rtpEndPoints,
    const Endpoints& rtcpEndPoints,
    memory::PacketPoolAllocator& allocator)
{
    return std::make_shared<TransportImpl>(jobmanager,
        srtpClientFactory,
        endpointIdHash,
        config,
        sctpConfig,
        bweConfig,
        rateControllerConfig,
        rtpEndPoints,
        rtcpEndPoints,
        allocator);
}

TransportImpl::TransportImpl(jobmanager::JobManager& jobmanager,
    SrtpClientFactory& srtpClientFactory,
    const size_t endpointIdHash,
    const config::Config& config,
    const sctp::SctpConfig& sctpConfig,
    const bwe::Config& bweConfig,
    const bwe::RateControllerConfig& rateControllerConfig,
    const Endpoints& rtpEndPoints,
    const Endpoints& rtcpEndPoints,
    memory::PacketPoolAllocator& allocator)
    : _isInitialized(false),
      _loggableId("Transport"),
      _endpointIdHash(endpointIdHash),
      _config(config),
      _srtpClient(srtpClientFactory.create(this)),
      _tcpEndpointFactory(nullptr),
      _jobCounter(0),
      _selectedRtp(nullptr),
      _selectedRtcp(nullptr),
      _dataReceiver(nullptr),
      _mainAllocator(allocator),
      _jobQueue(jobmanager, 256),
      _inboundMetrics(bweConfig.estimate.initialKbpsDownlink),
      _outboundMetrics(bweConfig.estimate.initialKbpsUplink),
      _outboundRembEstimateKbps(bweConfig.estimate.initialKbpsUplink),
      _sendRateTracker(utils::Time::ms * 100),
      _lastLogTimestamp(0),
      _rttNtp(0),
      _outboundSsrcCounters(256),
      _inboundSsrcCounters(16),
      _isRunning(true),
      _absSendTimeExtensionId(0),
      _videoRtxPayloadType(96),
      _sctpConfig(sctpConfig),
      _bwe(std::make_unique<bwe::BandwidthEstimator>(bweConfig)),
      _rateController(_loggableId.getInstanceId(), rateControllerConfig),
      _rtxProbeSsrc(0),
      _rtxProbeSequenceCounter(nullptr),
      _pacingInUse(false),
      _iceState(ice::IceSession::State::IDLE),
      _dtlsState(SrtpClient::State::IDLE),
      _isConnected(false),
      _rtcpProducer(_loggableId, _config, _outboundSsrcCounters, _inboundSsrcCounters, _mainAllocator, *this),
      _uplinkEstimationEnabled(false),
      _downlinkEstimationEnabled(false),
      _lastReceivedPacketTimestamp(0),
      _lastTickJobStartTimestamp(0)
{
    assert(endpointIdHash != 0);
    _tag[0] = 0;

    logger::info("SRTP client: %s", _loggableId.c_str(), _srtpClient->getLoggableId().c_str());
    assert(_srtpClient->isInitialized());
    if (!_srtpClient->isInitialized())
    {
        return;
    }
    _srtpClient->setSslWriteBioListener(this);

    for (auto& endpoint : rtpEndPoints)
    {
        _rtpEndpoints.push_back(endpoint);
        endpoint->registerDefaultListener(this);
    }
    for (auto& endpoint : rtcpEndPoints)
    {
        _rtcpEndpoints.push_back(endpoint);
        endpoint->registerDefaultListener(this);
    }

    if (_rtpEndpoints.size() == 1)
    {
        _selectedRtp = _rtpEndpoints.front().get();
    }
    if (_rtcpEndpoints.size() == 1)
    {
        _selectedRtcp = _rtcpEndpoints.front().get();
    }

    if (!_downlinkEstimationEnabled)
    {
        _inboundMetrics.estimatedKbps = Mbps100;
        _inboundMetrics.estimatedKbpsMin = Mbps100;
        _inboundMetrics.estimatedKbpsMax = Mbps100;
    }

    _isInitialized = true;
    logger::debug("started with %zu rtp + %zu rtcp endpoints. job count %u, bwe %s, rctl %s",
        _loggableId.c_str(),
        _rtpEndpoints.size(),
        _rtcpEndpoints.size(),
        _jobCounter.load(),
        _downlinkEstimationEnabled ? "on" : "off",
        _uplinkEstimationEnabled ? "on" : "off");
}

TransportImpl::TransportImpl(jobmanager::JobManager& jobmanager,
    SrtpClientFactory& srtpClientFactory,
    const size_t endpointIdHash,
    const config::Config& config,
    const sctp::SctpConfig& sctpConfig,
    const ice::IceConfig& iceConfig,
    const ice::IceRole iceRole,
    const bwe::Config& bweConfig,
    const bwe::RateControllerConfig& rateControllerConfig,
    const Endpoints& sharedEndpoints,
    const ServerEndpoints& tcpEndpoints,
    TcpEndpointFactory* tcpEndpointFactory,
    memory::PacketPoolAllocator& allocator,
    const size_t expectedInboundStreamCount,
    const size_t expectedOutboundStreamCount,
    const bool enableUplinkEstimation,
    const bool enableDownlinkEstimation)
    : _isInitialized(false),
      _loggableId("Transport"),
      _endpointIdHash(endpointIdHash),
      _config(config),
      _srtpClient(srtpClientFactory.create(this)),
      _tcpEndpointFactory(tcpEndpointFactory),
      _jobCounter(0),
      _selectedRtp(nullptr),
      _selectedRtcp(nullptr),
      _dataReceiver(nullptr),
      _mainAllocator(allocator),
      _jobQueue(jobmanager),
      _inboundMetrics(bweConfig.estimate.initialKbpsDownlink),
      _outboundMetrics(bweConfig.estimate.initialKbpsUplink),
      _outboundRembEstimateKbps(bweConfig.estimate.initialKbpsUplink),
      _sendRateTracker(utils::Time::ms * 100),
      _lastLogTimestamp(0),
      _rttNtp(0),
      _outboundSsrcCounters(expectedOutboundStreamCount),
      _inboundSsrcCounters(expectedInboundStreamCount),
      _isRunning(true),
      _absSendTimeExtensionId(0),
      _videoRtxPayloadType(96),
      _sctpConfig(sctpConfig),
      _bwe(std::make_unique<bwe::BandwidthEstimator>(bweConfig)),
      _rateController(_loggableId.getInstanceId(), rateControllerConfig),
      _rtxProbeSsrc(0),
      _rtxProbeSequenceCounter(nullptr),
      _pacingInUse(false),
      _iceState(ice::IceSession::State::IDLE),
      _dtlsState(SrtpClient::State::IDLE),
      _isConnected(false),
      _rtcpProducer(_loggableId, _config, _outboundSsrcCounters, _inboundSsrcCounters, _mainAllocator, *this),
      _uplinkEstimationEnabled(enableUplinkEstimation && _config.rctl.enable),
      _downlinkEstimationEnabled(enableDownlinkEstimation && _config.bwe.enable),
      _lastReceivedPacketTimestamp(0),
      _lastTickJobStartTimestamp(0)
{
    assert(endpointIdHash != 0);
    _tag[0] = 0;

    logger::info("SRTP client: %s", _loggableId.c_str(), _srtpClient->getLoggableId().c_str());
    assert(_srtpClient->isInitialized());
    if (!_srtpClient->isInitialized())
    {
        return;
    }
    _srtpClient->setSslWriteBioListener(this);

    _rtpIceSession = std::make_unique<ice::IceSession>(_loggableId.getInstanceId(),
        iceConfig,
        ice::IceComponent::RTP,
        iceRole,
        this);

    for (auto& endpoint : sharedEndpoints)
    {
        _rtpEndpoints.push_back(endpoint);
        _rtpIceSession->attachLocalEndpoint(endpoint.get());
        endpoint->registerListener(_rtpIceSession->getLocalCredentials().first, this);

        if (!iceConfig.publicIpv4.empty() && endpoint->getLocalPort().getFamily() == AF_INET)
        {
            auto addressPort = iceConfig.publicIpv4;
            addressPort.setPort(endpoint->getLocalPort().getPort());
            _rtpIceSession->addLocalCandidate(addressPort, endpoint.get());
        }
        if (!iceConfig.publicIpv6.empty() && endpoint->getLocalPort().getFamily() == AF_INET6)
        {
            auto addressPort = iceConfig.publicIpv6;
            addressPort.setPort(endpoint->getLocalPort().getPort());
            _rtpIceSession->addLocalCandidate(addressPort, endpoint.get());
        }
    }
    int index = 0;
    for (auto& endpoint : tcpEndpoints)
    {
        _rtpIceSession->addLocalTcpCandidate(ice::IceCandidate::Type::HOST,
            index++,
            endpoint->getLocalPort(),
            endpoint->getLocalPort(),
            ice::TcpType::PASSIVE);
        _tcpServerEndpoints.push_back(endpoint);
        endpoint->registerListener(_rtpIceSession->getLocalCredentials().first, this);

        if (!iceConfig.publicIpv4.empty() && endpoint->getLocalPort().getFamily() == AF_INET)
        {
            auto addressPort = iceConfig.publicIpv4;
            addressPort.setPort(endpoint->getLocalPort().getPort());
            _rtpIceSession->addLocalTcpCandidate(ice::IceCandidate::Type::SRFLX,
                index++,
                endpoint->getLocalPort(),
                addressPort,
                ice::TcpType::PASSIVE);

            if (config.ice.tcp.aliasPort != 0)
            {
                addressPort.setPort(config.ice.tcp.aliasPort);
                _rtpIceSession->addLocalTcpCandidate(ice::IceCandidate::Type::SRFLX,
                    index++,
                    endpoint->getLocalPort(),
                    addressPort,
                    ice::TcpType::PASSIVE);
            }
        }
        if (!iceConfig.publicIpv6.empty() && endpoint->getLocalPort().getFamily() == AF_INET6)
        {
            auto addressPort = iceConfig.publicIpv6;
            addressPort.setPort(endpoint->getLocalPort().getPort());
            _rtpIceSession->addLocalTcpCandidate(ice::IceCandidate::Type::SRFLX,
                index++,
                endpoint->getLocalPort(),
                addressPort,
                ice::TcpType::PASSIVE);

            if (config.ice.tcp.aliasPort != 0)
            {
                addressPort.setPort(config.ice.tcp.aliasPort);
                _rtpIceSession->addLocalTcpCandidate(ice::IceCandidate::Type::SRFLX,
                    index++,
                    endpoint->getLocalPort(),
                    addressPort,
                    ice::TcpType::PASSIVE);
            }
        }
    }

    _rtpIceSession->gatherLocalCandidates(iceConfig.gather.stunServers, utils::Time::getAbsoluteTime());

    if (!_downlinkEstimationEnabled)
    {
        _inboundMetrics.estimatedKbps = Mbps100;
        _inboundMetrics.estimatedKbpsMin = Mbps100;
        _inboundMetrics.estimatedKbpsMax = Mbps100;
    }

    _isInitialized = true;
    logger::debug("started with %zu udp + %zu tcp endpoints. job count %u, bwe %s, rctl %s",
        _loggableId.c_str(),
        _rtpEndpoints.size(),
        _tcpServerEndpoints.size(),
        _jobCounter.load(),
        _downlinkEstimationEnabled ? "on" : "off",
        _uplinkEstimationEnabled ? "on" : "off");

    if (!_config.bwe.packetLogLocation.get().empty())
    {
        const auto fileName = _config.bwe.packetLogLocation.get() + "/" + _loggableId.c_str();
        auto logFile = ::fopen(fileName.c_str(), "wr");
        _packetLogger.reset(new logger::PacketLoggerThread(logFile, 512));
    }
}
// When ref counter has been assigned, the inbound data from sockets can be handled
// as received job can be created.
bool TransportImpl::start()
{
    for (auto& endpoint : _rtpEndpoints)
    {
        endpoint->start();
    }
    for (auto& endpoint : _rtcpEndpoints)
    {
        endpoint->start();
    }
    return true;
}

TransportImpl::~TransportImpl()
{
    if (_jobCounter.load() > 0 || _isRunning || _jobQueue.getCount() > 0)
    {
        logger::warn("~TransportImpl not idle %p running%u jobcount %u, serialjobs %zu",
            _loggableId.c_str(),
            this,
            _isRunning.load(),
            _jobCounter.load(),
            _jobQueue.getCount());
    }
}

void TransportImpl::stop()
{
    _jobQueue.getJobManager().abortTimedJobs(getId());
    if (!_isRunning)
    {
        return;
    }
    logger::debug("stopping jobcount %u, running%u", _loggableId.c_str(), _jobCounter.load(), _isRunning.load());
    _isRunning = false;

    _jobQueue.post(_jobCounter, [this]() { internalShutdown(); });
    _jobQueue.post(_jobCounter, [this]() { internalUnregisterEndpoints(); });
}

void TransportImpl::internalShutdown()
{
    if (_rtpIceSession)
    {
        _rtpIceSession->stop();
    }

    _selectedRtp = nullptr;
    _srtpClient->stop();

    if (_sctpAssociation)
    {
        _sctpAssociation->close();
    }
    _isInitialized = false;
}

void TransportImpl::internalUnregisterEndpoints()
{
    for (auto& it : _outboundSsrcCounters)
    {
        it.second.stop();
    }

    for (auto& it : _inboundSsrcCounters)
    {
        it.second.stop();
    }

    logger::debug("Unregister from rtp endpoints jobcount %u, endpoints %zu",
        _loggableId.c_str(),
        _jobCounter.load(),
        _rtpEndpoints.size());
    for (auto& endpoint : _tcpServerEndpoints)
    {
        endpoint->unregisterListener(_rtpIceSession->getLocalCredentials().first, this);
    }

    // TODO a pending job for ice send could register a new callback to transport!
    for (auto& ep : _rtpEndpoints)
    {
        ep->unregisterListener(this);
    }
    for (auto& ep : _rtcpEndpoints)
    {
        ep->unregisterListener(this);
    }

    _jobQueue.getJobManager().abortTimedJobs(getId());
}

void TransportImpl::onRegistered(Endpoint& endpoint)
{
    ++_jobCounter;
    logger::debug("registered endpoint %s, %d", _loggableId.c_str(), endpoint.getName(), _jobCounter.load());
}

void TransportImpl::onUnregistered(Endpoint& endpoint)
{
    logger::debug("unregistered %s, %d", _loggableId.c_str(), endpoint.getName(), _jobCounter.load());
    _jobQueue.addJob<UnregisteredJob>(_jobCounter);
}

void TransportImpl::onServerPortUnregistered(ServerEndpoint& endpoint)
{
    logger::debug("unregistered %s, %d", _loggableId.c_str(), endpoint.getName(), _jobCounter.load());
    _jobQueue.addJob<UnregisteredJob>(_jobCounter);
}

void TransportImpl::onServerPortRegistered(ServerEndpoint& endpoint)
{
    logger::debug("registered %s, %d", _loggableId.c_str(), endpoint.getName(), _jobCounter.load());
    ++_jobCounter;
}

void TransportImpl::onRtpReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    if (!_jobQueue.post(_jobCounter,
            utils::bind(&TransportImpl::internalRtpReceived,
                this,
                std::ref(endpoint),
                source,
                utils::moveParam(packet),
                timestamp)))
    {
        logger::error("job queue full RTP", _loggableId.c_str());
    }
}

void TransportImpl::internalRtpReceived(Endpoint& endpoint,
    const SocketAddress source,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    _lastReceivedPacketTimestamp = timestamp;
    ++_inboundMetrics.packetCount;
    _inboundMetrics.bytesCount += packet->getLength();

    if (!_srtpClient->isConnected())
    {
        logger::debug("RTP received, dtls not connected yet", _loggableId.c_str());
        return;
    }

    if (_rtpIceSession)
    {
        _rtpIceSession->onPacketReceived(&endpoint, source, timestamp);
    }

    DataReceiver* const dataReceiver = _dataReceiver.load();
    if (!dataReceiver)
    {
        return;
    }

    const auto rtpHeader = rtp::RtpHeader::fromPacket(*packet);
    if (!rtpHeader)
    {
        return;
    }

    packet->endpointIdHash = _endpointIdHash;

    bool rembReady = false;
    if (_absSendTimeExtensionId)
    {
        if (_packetLogger)
        {
            _packetLogger->post(*packet, timestamp);
        }
        uint32_t absSendTime = 0;

        if (rtp::getTransmissionTimestamp(*packet, _absSendTimeExtensionId, absSendTime))
        {
            rembReady = doBandwidthEstimation(timestamp, utils::Optional<uint32_t>(absSendTime), packet->getLength());
        }
        else
        {
            rembReady = doBandwidthEstimation(timestamp, utils::Optional<uint32_t>(), packet->getLength());
        }
    }

    const uint32_t ssrc = rtpHeader->ssrc;
    const auto rtpFrequency = _audio.containsPayload(rtpHeader->payloadType) ? _audio.rtpFrequency : 90000;
    auto& ssrcState = getInboundSsrc(ssrc, rtpFrequency);

    ssrcState.onRtpReceived(*packet, timestamp);
    if (ssrcState.getCumulativeSnapshot().packets == 1 && !_downlinkEstimationEnabled)
    {
        rembReady = true; // force REMB to allow for high receive rate
    }

#if DEBUG_RTP
    if ((rtpHeader->sequenceNumber % 50) == 0)
    {
        RTP_LOG("received RTP ssrc %u, seq %u",
            _loggableId.c_str(),
            rtpHeader->ssrc.get(),
            rtpHeader->sequenceNumber.get());
    }
#endif

    if (utils::Time::diffGE(_rtcp.lastSendTime, timestamp, utils::Time::ms * 100) || rembReady)
    {
        sendReports(timestamp, rembReady);
    }

    if (ssrcState.currentRtpSource != source)
    {
        logger::debug("RTP ssrc %u from %s", _loggableId.c_str(), ssrc, source.toString().c_str());
        ssrcState.currentRtpSource = source;
    }

    const uint32_t extendedSequenceNumber = ssrcState.toExtendedSequenceNumber(rtpHeader->sequenceNumber.get());
    dataReceiver->onRtpPacketReceived(this, std::move(packet), extendedSequenceNumber, timestamp);
}

void TransportImpl::onDtlsReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    if (!_jobQueue.post(_jobCounter,
            utils::bind(&TransportImpl::internalDtlsReceived,
                this,
                std::ref(endpoint),
                source,
                utils::moveParam(packet),
                timestamp)))
    {
        logger::warn("job queue full DTLS", _loggableId.c_str());
    }
}

void TransportImpl::internalDtlsReceived(Endpoint& endpoint,
    const SocketAddress source,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    _lastReceivedPacketTimestamp = timestamp;
    ++_inboundMetrics.packetCount;
    _inboundMetrics.bytesCount += packet->getLength();

    if (_rtpIceSession)
    {
        _rtpIceSession->onPacketReceived(&endpoint, source, timestamp);
    }

    if (packet->get()[0] == DTLSContentType::applicationData)
    {
        DataReceiver* const dataReceiver = _dataReceiver.load();

        if (dataReceiver && _sctpServerPort && _srtpClient->unprotectApplicationData(*packet))
        {
            _sctpServerPort->onPacketReceived(packet->get(), packet->getLength(), timestamp);
        }
    }
    else
    {
        if (!_selectedRtp)
        {
            onIceCandidateChanged(_rtpIceSession.get(), &endpoint, source);
        }

        logger::debug("received DTLS protocol message from %s, %zu",
            _loggableId.c_str(),
            source.toString().c_str(),
            packet->getLength());
        _srtpClient->onMessageReceived(std::move(packet));
    }
}

void TransportImpl::onRtcpReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    if (!_jobQueue.post(_jobCounter,
            utils::bind(&TransportImpl::internalRtcpReceived,
                this,
                std::ref(endpoint),
                source,
                utils::moveParam(packet),
                timestamp)))
    {
        logger::warn("job queue full RTCP", _loggableId.c_str());
    }
}

void TransportImpl::internalRtcpReceived(Endpoint& endpoint,
    const SocketAddress source,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    _lastReceivedPacketTimestamp = timestamp;
    ++_inboundMetrics.packetCount;
    _inboundMetrics.bytesCount += packet->getLength();

    if (_rtpIceSession)
    {
        _rtpIceSession->onPacketReceived(&endpoint, source, timestamp);
    }

    if (!_srtpClient->isConnected())
    {
        logger::debug("RTCP received, dtls not connected yet", _loggableId.c_str());
        return;
    }

    auto* dataReceiver = _dataReceiver.load();
    if (!dataReceiver)
    {
        return;
    }

    const auto receiveTime = timestamp;

    _bwe->onUnmarkedTraffic(packet->getLength(), timestamp);

    const auto now = utils::Time::now();
    if (unprotect(*packet) && rtp::isValidRtcpPacket(*packet))
    {
        rtp::CompoundRtcpPacket compound(packet->get(), packet->getLength());
        for (auto& report : compound)
        {
            processRtcpReport(report, receiveTime, now);
        }

        dataReceiver->onRtcpPacketDecoded(this, std::move(packet), receiveTime);
        return;
    }
    else
    {
        auto header = rtp::RtcpHeader::fromPacket(*packet);
        logger::debug("failed to unprotect or invalid rtcp v%u, %u, len %u",
            _loggableId.c_str(),
            header->version,
            header->packetType,
            header->length.get());
    }
}

void TransportImpl::onIceReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    if (!_rtpIceSession)
    {
        return;
    }

    if (!_jobQueue.post(_jobCounter,
            utils::bind(&TransportImpl::internalIceReceived,
                this,
                std::ref(endpoint),
                source,
                utils::moveParam(packet),
                timestamp)))
    {
        logger::error("job queue full ICE", _loggableId.c_str());
    }
}

void TransportImpl::onIceTcpConnect(std::shared_ptr<Endpoint> endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    if (_rtpIceSession && _isRunning)
    {
        const bool posted = _jobQueue.post(_jobCounter,
            utils::bind(&TransportImpl::internalIceTcpConnect,
                this,
                endpoint,
                source,
                utils::moveParam(packet),
                timestamp));

        if (!posted)
        {
            logger::error("job queue full ICE, TCP connect", _loggableId.c_str());
        }
    }
}

void TransportImpl::internalIceTcpConnect(std::shared_ptr<Endpoint> endpoint,
    const SocketAddress source,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    if (!_isRunning)
    {
        return;
    }

    logger::debug("new tcp connection %s", _loggableId.c_str(), endpoint->getName());

    endpoint->registerDefaultListener(this);
    _rtpEndpoints.push_back(endpoint);
    internalIceReceived(*endpoint, source, std::move(packet), timestamp);
}

void TransportImpl::internalIceReceived(Endpoint& endpoint,
    const SocketAddress source,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    _lastReceivedPacketTimestamp = timestamp;
    ++_inboundMetrics.packetCount;
    _inboundMetrics.bytesCount += packet->getLength();
    _bwe->onUnmarkedTraffic(packet->getLength(), timestamp);

    if (!_rtpIceSession)
    {
        return;
    }

    const auto msg = ice::StunMessage::fromPtr(packet->get());
    if (ice::isResponse(packet->get()))
    {
        const auto responseCode = msg->getAttribute<ice::StunError>(ice::StunAttribute::ERROR_CODE);
        if (responseCode && responseCode->isValid())
        {
            const auto code = responseCode->getCode();
            if (code != 200)
            {
                logger::warn("ICE response %d received, '%s', from %s",
                    getLoggableId().c_str(),
                    code,
                    responseCode->getPhrase().c_str(),
                    endpoint.getName());
            }
        }
    }

    auto dataReceiver = _dataReceiver.load();
    if (_rtpIceSession->isAttached(&endpoint))
    {
        if (!_rtpIceSession->isIceAuthentic(packet->get(), packet->getLength()))
        {
            // TODO attack, add metric for this
            return;
        }

        auto timeout = _rtpIceSession->nextTimeout(timestamp);
        _rtpIceSession->onStunPacketReceived(&endpoint, source, packet->get(), packet->getLength(), timestamp);
        if (dataReceiver)
        {
            dataReceiver->onIceReceived(this, timestamp);
        }
        auto newTimeout = _rtpIceSession->nextTimeout(timestamp);
        if (newTimeout != timeout)
        {
            _jobQueue.getJobManager().replaceTimedJob<IceTimerTriggerJob>(getId(),
                IceTimerTriggerJob::TIMER_ID,
                newTimeout / 1000,
                *this,
                *_rtpIceSession);
        }
    }
    else if (_rtpIceSession && endpoint.getTransportType() == ice::TransportType::TCP)
    {
        // It is possible that a ice packet has been enqueued while we were discarding TCP candidates.
        // Check if it CONNECTED to avoid IceSession to store a reference that will be dangling soon
        if (Endpoint::State::CONNECTED == endpoint.getState())
        {
            _rtpIceSession->onStunPacketReceived(&endpoint, source, packet->get(), packet->getLength(), timestamp);
            if (dataReceiver)
            {
                dataReceiver->onIceReceived(this, timestamp);
            }
        }
    }
}

void TransportImpl::onTcpDisconnect(Endpoint& endpoint)
{
    _jobQueue.post([&]() { _rtpIceSession->onTcpDisconnect(&endpoint); });
}

void TransportImpl::onEndpointStopped(Endpoint* endpoint)
{
    _jobQueue.post(utils::bind(&TransportImpl::onTcpEndpointStoppedInternal, this, endpoint));
}

void TransportImpl::onTcpEndpointStoppedInternal(Endpoint* endpoint)
{
    auto it = std::find_if(_rtpEndpoints.begin(), _rtpEndpoints.end(), [endpoint](const auto& ep) {
        return static_cast<const transport::Endpoint*>(ep.get()) == endpoint;
    });

    if (it != _rtpEndpoints.end())
    {
        std::iter_swap(it, std::prev(_rtpEndpoints.end()));
        _rtpEndpoints.pop_back();
    }

    --_jobCounter;
}

namespace
{
uint32_t processReportBlocks(const uint32_t count,
    const rtp::ReportBlock blocks[],
    concurrency::MpmcHashmap32<uint32_t, RtpSenderState>& outboundSsrcCounters,
    const uint64_t timestamp,
    const uint32_t wallclockNtp32,
    bwe::RateController& rateController)
{
    uint32_t rttNtp = ~0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto& block = blocks[i];
        auto it = outboundSsrcCounters.find(block.ssrc);
        if (it != outboundSsrcCounters.cend())
        {
            it->second.onReceiverBlockReceived(timestamp, wallclockNtp32, block);
            rttNtp = std::min(rttNtp, it->second.getRttNtp());
            rateController.onReportBlockReceived(block.ssrc,
                block.extendedSeqNoReceived,
                block.loss.getCumulativeLoss(),
                block.lastSR,
                block.delaySinceLastSR);
        }
    }

    if (count > 0)
    {
        rateController.onReportReceived(timestamp, count, blocks, rttNtp);
    }

    return rttNtp;
}
} // namespace

void TransportImpl::processRtcpReport(const rtp::RtcpHeader& header,
    const uint64_t timestamp,
    std::__1::chrono::system_clock::time_point wallClock)
{
    uint32_t rttNtp = ~0u;
    uint32_t recvNtp32 = utils::Time::toNtp32(wallClock);

    if (header.packetType == rtp::RtcpPacketType::SENDER_REPORT)
    {
        const auto senderReport = rtp::RtcpSenderReport::fromPtr(&header, header.size());
        rttNtp = processReportBlocks(senderReport->header.fmtCount,
            senderReport->reportBlocks,
            _outboundSsrcCounters,
            timestamp,
            recvNtp32,
            _rateController);

        if (_uplinkEstimationEnabled)
        {
            _outboundMetrics.estimatedKbps = _rateController.getTargetRate();
        }

        auto& ssrcState = getInboundSsrc(senderReport->ssrc, 0);
        ssrcState.onRtcpReceived(header, timestamp, utils::Time::toNtp(wallClock));
    }
    else if (header.packetType == rtp::RtcpPacketType::RECEIVER_REPORT)
    {
        const auto receiverReport = rtp::RtcpReceiverReport::fromPtr(&header, header.size());
        rttNtp = processReportBlocks(receiverReport->header.fmtCount,
            receiverReport->reportBlocks,
            _outboundSsrcCounters,
            timestamp,
            recvNtp32,
            _rateController);
        if (_uplinkEstimationEnabled)
        {
            _outboundMetrics.estimatedKbps = _rateController.getTargetRate();
        }
    }
    else if (rtp::isRemb(&header))
    {
        const auto& remb = reinterpret_cast<const rtp::RtcpRembFeedback&>(header);
        const uint32_t estimatedKbps = remb.getBitrate() / 1000LLU;
        _outboundRembEstimateKbps = estimatedKbps;

        if (!_uplinkEstimationEnabled)
        {
            _outboundMetrics.estimatedKbps = estimatedKbps;
        }
    }

    if (rttNtp < ~0u)
    {
        _rttNtp = rttNtp;
    }
}

/** Adds ssrc tracker if it does not exist
 *
 */
RtpReceiveState& TransportImpl::getInboundSsrc(const uint32_t ssrc, uint32_t rtpFrequency)
{
    auto ssrcIt = _inboundSsrcCounters.find(ssrc);
    if (ssrcIt != _inboundSsrcCounters.cend())
    {
        if (rtpFrequency != 0)
        {
            ssrcIt->second.setRtpFrequency(rtpFrequency);
        }
        return ssrcIt->second;
    }

    if (_inboundSsrcCounters.size() >= _inboundSsrcCounters.capacity())
    {
        auto nominee = _inboundSsrcCounters.begin();
        for (auto it = _inboundSsrcCounters.begin(); it != _inboundSsrcCounters.end(); ++it)
        {
            if (utils::Time::diffGT(it->second.getLastActive(), nominee->second.getLastActive(), 0))
            {
                nominee = it;
            }
        }
        logger::warn("unexpected number of inbound streams. Discarding %u", _loggableId.c_str(), nominee->first);
        _inboundSsrcCounters.erase(nominee->first);

        auto pairIt = _inboundSsrcCounters.emplace(ssrc, _config, rtpFrequency);
        return pairIt.first->second;
    }
    else
    {
        auto pairIt = _inboundSsrcCounters.emplace(ssrc, _config, rtpFrequency);
        return pairIt.first->second;
    }
}

RtpSenderState& TransportImpl::getOutboundSsrc(const uint32_t ssrc, const uint32_t rtpFrequency)
{
    auto ssrcIt = _outboundSsrcCounters.find(ssrc);
    if (ssrcIt != _outboundSsrcCounters.cend())
    {
        return ssrcIt->second;
    }

    if (_outboundSsrcCounters.size() >= _outboundSsrcCounters.capacity())
    {
        auto nominee = _outboundSsrcCounters.begin();
        for (auto it = _outboundSsrcCounters.begin(); it != _outboundSsrcCounters.end(); ++it)
        {
            if (utils::Time::diffGT(it->second.getLastSendTime(), nominee->second.getLastSendTime(), 0))
            {
                nominee = it;
            }
        }
        logger::warn("unexpected number of outbound streams. Discarding %u", _loggableId.c_str(), nominee->first);
        _outboundSsrcCounters.erase(nominee->first);
    }

    auto pairIt = _outboundSsrcCounters.emplace(ssrc, rtpFrequency, _config);
    return pairIt.first->second; // TODO error check
}

// return true if urgent feedback is needed
bool TransportImpl::doBandwidthEstimation(const uint64_t receiveTime,
    const utils::Optional<uint32_t>& absSendTime,
    const uint32_t packetSize)
{
    if (!_downlinkEstimationEnabled)
    {
        _inboundMetrics.estimatedKbps = Mbps100;
        _inboundMetrics.estimatedKbpsMin = Mbps100;
        _inboundMetrics.estimatedKbpsMax = Mbps100;
        _bwe->onUnmarkedTraffic(packetSize, receiveTime);
        return false;
    }

    if (!absSendTime.isSet())
    {
        _bwe->onUnmarkedTraffic(packetSize, receiveTime);
        return false;
    }

    const auto sendTime = _sendTimeTracker.toAbsoluteTime(absSendTime.get(), receiveTime);

    _bwe->update(packetSize, sendTime, receiveTime);
    const uint32_t newEstimate = static_cast<uint32_t>(_bwe->getEstimate(receiveTime));

    _inboundMetrics.estimatedKbps = newEstimate;
    if (_config.bwe.logDownlinkEstimates)
    {
        _inboundMetrics.estimatedKbpsMin = std::min(_inboundMetrics.estimatedKbpsMin.load(), newEstimate);
        _inboundMetrics.estimatedKbpsMax = std::max(_inboundMetrics.estimatedKbpsMax.load(), newEstimate);
    }
    if (!_selectedRtcp)
    {
        return false;
    }

    const uint32_t delta = newEstimate - _rtcp.lastReportedEstimateKbps;
    const uint32_t relativeDelta = delta / std::max(100u, _rtcp.lastReportedEstimateKbps);
    if (relativeDelta < -0.1 ||
        ((relativeDelta > 0.15 || delta > 500) &&
            utils::Time::diffGT(_rtcp.lastSendTime, receiveTime, utils::Time::ms * 300)))
    {
        return true;
    }
    return false;
}

uint32_t TransportImpl::getSenderLossCount() const
{
    uint32_t lossCount = 0;
    for (auto& context : _outboundSsrcCounters)
    {
        lossCount += context.second.getSummary().lostPackets;
    }
    return lossCount;
}

PacketCounters TransportImpl::getAudioSendCounters(uint64_t idleTimestamp) const
{
    PacketCounters counters;
    for (auto& it : _outboundSsrcCounters)
    {
        if (it.second.payloadType == _audio.payloadType)
        {
            if (utils::Time::diffGE(idleTimestamp, it.second.getLastSendTime(), 0))
            {
                counters += it.second.getCounters();
                ++counters.activeStreamCount;
            }
        }
    }
    return counters;
}

void TransportImpl::getReportSummary(std::unordered_map<uint32_t, ReportSummary>& outReportSummary) const
{
    for (auto& outboundSsrcCountersEntry : _outboundSsrcCounters)
    {
        outReportSummary.emplace(outboundSsrcCountersEntry.first, outboundSsrcCountersEntry.second.getSummary());
    }
}

PacketCounters TransportImpl::getVideoSendCounters(uint64_t idleTimestamp) const
{
    PacketCounters counters;
    for (auto& it : _outboundSsrcCounters)
    {
        if (it.second.payloadType != _audio.payloadType)
        {
            if (utils::Time::diffGE(idleTimestamp, it.second.getLastSendTime(), 0))
            {
                counters += it.second.getCounters();
                ++counters.activeStreamCount;
            }
        }
    }
    return counters;
}

PacketCounters TransportImpl::getAudioReceiveCounters(uint64_t idleTimestamp) const
{
    PacketCounters total;
    for (auto& it : _inboundSsrcCounters)
    {
        if (it.second.payloadType == _audio.payloadType)
        {
            if (utils::Time::diffGE(idleTimestamp, it.second.getLastActive(), 0))
            {
                total += it.second.getCounters();
                ++total.activeStreamCount;
            }
        }
    }
    return total;
}

PacketCounters TransportImpl::getVideoReceiveCounters(uint64_t idleTimestamp) const
{
    PacketCounters total;
    for (auto& it : _inboundSsrcCounters)
    {
        if (it.second.payloadType != _audio.payloadType)
        {
            if (utils::Time::diffGE(idleTimestamp, it.second.getLastActive(), 0))
            {
                total += it.second.getCounters();
                ++total.activeStreamCount;
            }
        }
    }
    return total;
}

PacketCounters TransportImpl::getCumulativeAudioReceiveCounters() const
{
    PacketCounters total;
    for (auto& it : _inboundSsrcCounters)
    {
        if (it.second.payloadType == _audio.payloadType)
        {
            total += it.second.getCumulativeCounters();
        }
    }
    return total;
}

PacketCounters TransportImpl::getCumulativeVideoReceiveCounters() const
{
    PacketCounters total;
    for (auto& it : _inboundSsrcCounters)
    {
        if (it.second.payloadType != _audio.payloadType)
        {
            total += it.second.getCumulativeCounters();
        }
    }
    return total;
}

PacketCounters TransportImpl::getCumulativeReceiveCounters(uint32_t ssrc) const
{
    auto it = _inboundSsrcCounters.find(ssrc);
    if (it != _inboundSsrcCounters.cend())
    {
        return it->second.getCumulativeCounters();
    }

    return PacketCounters();
}

uint32_t TransportImpl::getUplinkEstimateKbps() const
{
    return _outboundMetrics.estimatedKbps;
}

uint32_t TransportImpl::getDownlinkEstimateKbps() const
{
    return _inboundMetrics.estimatedKbps;
}

uint32_t TransportImpl::getPacingQueueCount() const
{
    return _pacingQueueStats.pacingQueueSize;
}

uint32_t TransportImpl::getRtxPacingQueueCount() const
{
    return _pacingQueueStats.rtxPacingQueueSize;
}

void TransportImpl::doProtectAndSend(uint64_t timestamp,
    memory::UniquePacket packet,
    const SocketAddress& target,
    Endpoint* endpoint)
{
    _outboundMetrics.bytesCount += packet->getLength();
    ++_outboundMetrics.packetCount;

    assert(packet->getLength() + 24 <= _config.mtu);
    if (endpoint && _srtpClient->protect(*packet))
    {
        _sendRateTracker.update(packet->getLength(), timestamp);
        endpoint->sendTo(target, std::move(packet));
    }
}

void TransportImpl::sendPadding(uint64_t timestamp)
{
    if (!_uplinkEstimationEnabled || !_rtxProbeSequenceCounter)
    {
        return;
    }

    drainPacingBuffer(timestamp, DrainPacingBufferMode::UseBudget);

    uint16_t padding = 0;
    auto& rtxSenderState = getOutboundSsrc(_rtxProbeSsrc, 90000);
    auto rtpTimeStamp = rtxSenderState.getRtpTimestamp(timestamp);
    auto paddingCount = _rateController.getPadding(timestamp, 0, padding);
    for (uint32_t i = 0; padding > 100 && i < paddingCount && _selectedRtp; ++i)
    {
        auto padPacket = memory::makeUniquePacket(_mainAllocator);
        if (padPacket)
        {
            padPacket->clear();
            padPacket->setLength(padding);
            auto padRtpHeader = rtp::RtpHeader::create(*padPacket);
            padRtpHeader->ssrc = _rtxProbeSsrc;
            padRtpHeader->payloadType = _videoRtxPayloadType;
            padRtpHeader->timestamp = rtpTimeStamp;
            padRtpHeader->sequenceNumber = ++(*_rtxProbeSequenceCounter) & 0xFFFF;
            padRtpHeader->padding = 1;
            padPacket->get()[padPacket->getLength() - 1] = 0x01;
            if (_absSendTimeExtensionId)
            {
                padRtpHeader->extension = 1;
                rtp::RtpHeaderExtension extensionHead;
                rtp::GeneralExtension1Byteheader absSendTime(_absSendTimeExtensionId, 3);
                auto cursor = extensionHead.extensions().begin();
                extensionHead.addExtension(cursor, absSendTime);
                padRtpHeader->setExtensions(extensionHead);
            }

            // fake a really old packet
            reinterpret_cast<uint16_t*>(padRtpHeader->getPayload())[0] = hton<uint16_t>(23033);
            protectAndSendRtp(timestamp, std::move(padPacket));
        }
        else
        {
            break;
        }
    }
}

namespace
{
template <typename T>
void clearPacingQueueIfFull(T& pacingQueue)
{
    if (pacingQueue.full())
    {
        pacingQueue.clear();
    }
}
} // namespace
void TransportImpl::protectAndSend(memory::UniquePacket packet)
{
    DBGCHECK_SINGLETHREADED(_singleThreadMutex);

    assert(_srtpClient);
    const auto timestamp = utils::Time::getAbsoluteTime();

    if (!_srtpClient->isConnected() || !_selectedRtp)
    {
        if (_isRunning)
        {
            logger::debug("dropping packet, not connected", _loggableId.c_str());
        }
        return;
    }

    if (rtp::isRtpPacket(*packet))
    {
        sendReports(timestamp);
        const auto* rtpHeader = rtp::RtpHeader::fromPacket(*packet);
        const auto payloadType = rtpHeader->payloadType;
        const auto isAudio = (payloadType <= 8 || _audio.containsPayload(payloadType));

        clearPacingQueueIfFull(_pacingQueue);
        clearPacingQueueIfFull(_rtxPacingQueue);
        if (payloadType == _videoRtxPayloadType)
        {
            _rtxPacingQueue.push_front(std::move(packet));
        }
        else if (!isAudio)
        {
            _pacingQueue.push_front(std::move(packet));
        }
        else
        {
            protectAndSendRtp(timestamp, std::move(packet));
        }

        doRunTick(timestamp);
    }
    else if (_selectedRtcp)
    {
        sendRtcp(std::move(packet), timestamp);
    }
}

void TransportImpl::sendReports(uint64_t timestamp, bool rembReady)
{
    if (_config.bwe.logDownlinkEstimates && timestamp - _lastLogTimestamp >= utils::Time::sec * 5)
    {
        const auto oldMin = _inboundMetrics.estimatedKbpsMin.load();
        const auto oldMax = _inboundMetrics.estimatedKbpsMax.load();

        double maxJitter = 0;
        for (auto& inboundSsrc : _inboundSsrcCounters)
        {
            maxJitter = std::max(static_cast<double>(inboundSsrc.second.getJitter()) /
                    static_cast<double>(inboundSsrc.second.getRtpFrequency()),
                maxJitter);
        }

        logger::info("Estimates 5s, Downlink %u - %ukbps, rate %.1fkbps, Uplink rctl %.0fkbps, rate %.1fkbps, remb "
                     "%ukbps, rtt %.1fms, pacingQ %zu, rtpProbingEnabled %s, maxjitter %.2f",
            _loggableId.c_str(),
            oldMin,
            oldMax,
            _bwe->getReceiveRate(timestamp),
            _rateController.getTargetRate(),
            _sendRateTracker.get(timestamp, utils::Time::ms * 600) * 8 * utils::Time::ms,
            _outboundRembEstimateKbps,
            _rttNtp * 1000.0 / 0x10000,
            _pacingQueue.size() + _rtxPacingQueue.size(),
            _rateController.isRtpProbingEnabled() ? "t" : "f",
            maxJitter * 1000.0);

        if (_downlinkEstimationEnabled)
        {
            _inboundMetrics.estimatedKbpsMin = 0xFFFFFFFF;
            _inboundMetrics.estimatedKbpsMax = 0;
        }
        _lastLogTimestamp = timestamp;
    }

    utils::Optional<uint64_t> rembMediaBps;
    if (rembReady)
    {
        rembMediaBps.set(_inboundMetrics.estimatedKbps * 1000 * (1.0 - _config.bwe.packetOverhead));
    }

    const bool isRembSent = _rtcpProducer.sendReports(timestamp, rembMediaBps);
    if (isRembSent)
    {
        assert(rembMediaBps.isSet());
        assert(rembReady);
        _rtcp.lastReportedEstimateKbps = _inboundMetrics.estimatedKbps;
    }
}

void TransportImpl::protectAndSendRtp(uint64_t timestamp, memory::UniquePacket packet)
{
    const auto* rtpHeader = rtp::RtpHeader::fromPacket(*packet);
    const auto payloadType = rtpHeader->payloadType;
    const auto isAudio = (payloadType <= 8 || _audio.containsPayload(payloadType));
    const uint32_t rtpFrequency = isAudio ? _audio.rtpFrequency : 90000;

    if (_absSendTimeExtensionId)
    {
        // telephone events does not have transmission timestamp
        if (!_audio.telephoneEventPayloadType.isSet() ||
            _audio.telephoneEventPayloadType.get() != rtpHeader->payloadType)
        {
            rtp::setTransmissionTimestamp(*packet, _absSendTimeExtensionId, timestamp);
        }
    }

    auto& ssrcState = getOutboundSsrc(rtpHeader->ssrc, rtpFrequency);

    ssrcState.onRtpSent(timestamp, *packet);
    if (_uplinkEstimationEnabled)
    {
        _rateController.onRtpSent(timestamp, rtpHeader->ssrc, rtpHeader->sequenceNumber, packet->getLength());
    }

#if DEBUG_RTP
    if ((rtpHeader->sequenceNumber % 50) == 0)
    {
        RTP_LOG("sent RTP ssrc %u, seq %u",
            _loggableId.c_str(),
            rtpHeader->ssrc.get(),
            rtpHeader->sequenceNumber.get());
    }
#endif

    doProtectAndSend(timestamp, std::move(packet), _peerRtpPort, _selectedRtp);
}

void TransportImpl::sendRtcp(memory::UniquePacket rtcpPacket, const uint64_t timestamp)
{
    auto* report = rtp::RtcpReport::fromPacket(*rtcpPacket);

    if (!report ||
        (report->header.packetType != rtp::RtcpPacketType::SENDER_REPORT &&
            report->header.packetType != rtp::RtcpPacketType::RECEIVER_REPORT))
    {
        uint8_t originalReport[rtcpPacket->getLength()];
        std::memcpy(originalReport, rtcpPacket->get(), rtcpPacket->getLength());
        const int originalReportLength = rtcpPacket->getLength();
        rtcpPacket->setLength(0);

        auto* receiveReport = rtp::RtcpReceiverReport::create(rtcpPacket->get());
        if (_inboundSsrcCounters.size() > 0)
        {
            receiveReport->ssrc = _inboundSsrcCounters.begin()->first;
        }
        rtcpPacket->setLength(receiveReport->header.size());
        if (rtcpPacket->getLength() + originalReportLength > memory::Packet::size)
        {
            assert(false);
            return;
        }

        std::memcpy(rtcpPacket->get() + rtcpPacket->getLength(), originalReport, originalReportLength);
        rtcpPacket->setLength(rtcpPacket->getLength() + originalReportLength);
    }

    if (!rtp::CompoundRtcpPacket::isValid(rtcpPacket->get(), rtcpPacket->getLength()))
    {
        logger::warn("corrupt outbound rtcp packet", _loggableId.c_str());
        return;
    }

    onSendingRtcp(*rtcpPacket, timestamp);
    doProtectAndSend(timestamp, std::move(rtcpPacket), _peerRtcpPort, _selectedRtcp);
}

void TransportImpl::onSendingRtcp(const memory::Packet& rtcpPacket, const uint64_t timestamp)
{
    const rtp::CompoundRtcpPacket compound(rtcpPacket.get(), rtcpPacket.getLength());

    uint16_t remainingBytes = rtcpPacket.getLength();
    for (auto& header : compound)
    {
        const auto ssrc = header.getReporterSsrc();

        if (header.packetType == rtp::SENDER_REPORT)
        {
            auto rtpStateIt = _outboundSsrcCounters.find(ssrc);
            if (rtpStateIt != _outboundSsrcCounters.end())
            {
                rtpStateIt->second.onRtcpSent(timestamp, &header, rtcpPacket.getLength());
            }
            const auto* senderReport = rtp::RtcpSenderReport::fromPtr(&header, remainingBytes);
            if (senderReport && _uplinkEstimationEnabled)
            {
                // only one SR is allowed in packet
                _rateController.onSenderReportSent(timestamp,
                    ssrc,
                    (senderReport->ntpSeconds.get() << 16) | (senderReport->ntpFractions.get() >> 16),
                    rtcpPacket.getLength() + 12);
            }
        }

        remainingBytes -= header.size();
    }

    _rtcp.lastSendTime = timestamp;
}

bool TransportImpl::unprotect(memory::Packet& packet)
{
    if (_srtpClient->isConnected())
    {
        return _srtpClient->unprotect(packet);
    }
    return false;
}

void TransportImpl::removeSrtpLocalSsrc(const uint32_t ssrc)
{
    if (_srtpClient->isInitialized())
    {
        _srtpClient->removeLocalSsrc(ssrc);
    }
}

bool TransportImpl::setSrtpRemoteRolloverCounter(const uint32_t ssrc, const uint32_t rolloverCounter)
{
    if (_srtpClient->isConnected())
    {
        return _srtpClient->setRemoteRolloverCounter(ssrc, rolloverCounter);
    }
    return false;
}

bool TransportImpl::isGatheringComplete() const
{
    return (!_rtpIceSession || _rtpIceSession->getState() != ice::IceSession::State::GATHERING);
}

bool TransportImpl::isConnected()
{
    return _isConnected;
}

void TransportImpl::connect()
{
    // must be serialized after set Ice and set Dtls
    // otherwise transport will not know when we are connected and may start connect too early
    _jobQueue.post(_jobCounter, [this]() { doConnect(); });
}

void TransportImpl::doConnect()
{
    if (!isInitialized())
    {
        return;
    }

    if (_rtpIceSession)
    {
        _jobQueue.addJob<IceStartJob>(*this, *_rtpIceSession);
    }
    if (!_rtpIceSession && _srtpClient->getState() == SrtpClient::State::READY)
    {
        DtlsTimerJob::start(_jobQueue, *this, *_srtpClient, utils::Time::ms * 10);
    }
    auto* dataReceiver = _dataReceiver.load();
    if (dataReceiver && isConnected() && !_peerRtpPort.empty())
    {
        dataReceiver->onConnected(this);
    }
}

ice::IceCandidates TransportImpl::getLocalCandidates()
{
    if (_rtpIceSession)
    {
        return _rtpIceSession->getLocalCandidates();
    }

    return std::vector<ice::IceCandidate>();
}

std::pair<std::string, std::string> TransportImpl::getLocalIceCredentials()
{
    if (!_rtpIceSession)
    {
        logger::error("extracting credentials before ice session is created", getLoggableId().c_str());
        return std::make_pair("", "");
    }
    return _rtpIceSession->getLocalCredentials();
}

SocketAddress TransportImpl::getLocalRtpPort() const
{
    if (_selectedRtp)
    {
        return _selectedRtp->getLocalPort();
    }
    return SocketAddress();
}

bool TransportImpl::setRemotePeer(const SocketAddress& target)
{
    _peerRtpPort = target;
    _peerRtcpPort = target;
    _peerRtcpPort.setPort(target.getPort() + 1);
    for (auto& endpoint : _rtpEndpoints)
    {
        endpoint->registerListener(_peerRtpPort, this);
    }

    for (auto& endpoint : _rtcpEndpoints)
    {
        endpoint->registerListener(_peerRtcpPort, this);
    }
    return true;
}

// Only support for RTP component, no rtcp component
void TransportImpl::setRemoteIce(const std::pair<std::string, std::string>& credentials,
    const ice::IceCandidates& candidates,
    memory::AudioPacketPoolAllocator& allocator)
{
    if (_rtpIceSession)
    {
        assert(!credentials.second.empty());
        _jobQueue.addJob<IceSetRemoteJob>(*this, credentials, candidates, allocator);
    }
}

void TransportImpl::addRemoteIceCandidate(const ice::IceCandidate& candidate)
{
    if (_rtpIceSession)
    {
        auto* iceSession = _rtpIceSession.get();
        _jobQueue.post(_jobCounter, [iceSession, candidate]() {
            iceSession->addRemoteCandidate(candidate);
            if (iceSession->getState() >= ice::IceSession::State::CONNECTING)
            {
                iceSession->probeRemoteCandidates(iceSession->getRole(), utils::Time::getAbsoluteTime());
            }
        });
    }
}

void TransportImpl::doSetRemoteIce(const memory::AudioPacket& credentialPacket,
    const memory::AudioPacket& candidatesPacket)
{
    if (!_rtpIceSession)
    {
        return;
    }

    auto& credentials = *reinterpret_cast<const IceCredentials*>(credentialPacket.get());
    _rtpIceSession->setRemoteCredentials(std::pair<std::string, std::string>(credentials.ufrag, credentials.password));

    memory::MemoryFile stream(candidatesPacket.get(), candidatesPacket.getLength());
    while (stream.isGood())
    {
        ice::IceCandidate candidate;
        stream >> candidate;
        if (stream.isGood() && candidate.component == ice::IceComponent::RTP && !candidate.empty())
        {
            if (candidate.transportType == ice::TransportType::UDP)
            {
                _rtpIceSession->addRemoteCandidate(candidate);
            }
            else if (candidate.transportType == ice::TransportType::TCP && candidate.tcpType == ice::TcpType::PASSIVE &&
                _tcpEndpointFactory)
            {
                auto endpoints = _tcpEndpointFactory->createTcpEndpoints(candidate.address.getFamily());
                for (auto& endpoint : endpoints)
                {
                    endpoint->registerDefaultListener(this);
                    _rtpEndpoints.push_back(endpoint);

                    _rtpIceSession->addRemoteTcpPassiveCandidate(candidate, endpoint.get());
                }
            }
        }
    }
}

void TransportImpl::asyncSetRemoteDtlsFingerprint(const std::string& fingerprintType,
    const std::string& fingerprintHash,
    const bool dtlsClientSide)
{
    if (_srtpClient->getState() == SrtpClient::State::IDLE)
    {
        _jobQueue.addJob<DtlsSetRemoteJob>(*this,
            *_srtpClient,
            fingerprintHash.c_str(),
            fingerprintType.c_str(),
            dtlsClientSide,
            _mainAllocator);
    }
}

void TransportImpl::asyncDisableSrtp()
{
    _jobQueue.addJob<DtlsSetRemoteJob>(*this, *_srtpClient, "", "", false, _mainAllocator);
}

void TransportImpl::setSctp(const uint16_t localPort, const uint16_t remotePort)
{
    _remoteSctpPort.set(remotePort);
    if (!_sctpServerPort)
    {
        _sctpServerPort = std::make_unique<sctp::SctpServerPort>(_loggableId.getInstanceId(),
            this,
            this,
            localPort,
            _sctpConfig,
            utils::Time::getAbsoluteTime());
    }
}

void TransportImpl::onIceCandidateChanged(ice::IceSession* session,
    ice::IceEndpoint* endpoint,
    const SocketAddress& sourcePort)
{
    if (_selectedRtp == nullptr && session->getState() >= ice::IceSession::State::READY)
    {
        _selectedRtp = static_cast<transport::Endpoint*>(endpoint); // temporary selection
        _selectedRtcp = _selectedRtp;
        _peerRtpPort = sourcePort;
        _peerRtcpPort = sourcePort;
        _transportType.store(utils::Optional<ice::TransportType>(endpoint->getTransportType()));

        logger::info("temporary candidate selected %s %s, %s - %s",
            _loggableId.c_str(),
            endpoint->getLocalPort().getFamilyString().c_str(),
            ice::toString(endpoint->getTransportType()).c_str(),
            endpoint->getLocalPort().toFixedString().c_str(),
            maybeMasked(sourcePort).c_str());

        if (_srtpClient->getState() == SrtpClient::State::READY)
        {
            DtlsTimerJob::start(_jobQueue, *this, *_srtpClient, 1);
        }

        _isConnected = (_selectedRtp && _srtpClient->isConnected());
    }
    else if (_selectedRtp && endpoint != _selectedRtp)
    {
        _selectedRtp = static_cast<transport::Endpoint*>(endpoint);
        _selectedRtcp = _selectedRtp;
        _peerRtpPort = sourcePort;
        _peerRtcpPort = sourcePort;
        _transportType.store(utils::Optional<ice::TransportType>(endpoint->getTransportType()));

        logger::info("switching ICE candidate pair to %s %s, %s - %s",
            _loggableId.c_str(),
            endpoint->getLocalPort().getFamilyString().c_str(),
            ice::toString(endpoint->getTransportType()).c_str(),
            endpoint->getLocalPort().toFixedString().c_str(),
            maybeMasked(sourcePort).c_str());
    }
}

void TransportImpl::onIceCompleted(ice::IceSession* session)
{
    logger::debug("ice completed %s",
        getLoggableId().c_str(),
        session->getState() == ice::IceSession::State::CONNECTED ? "successfully" : "failure");
}

void TransportImpl::onIceStateChanged(ice::IceSession* session, const ice::IceSession::State state)
{
    _iceState = state;
    _isConnected = (_selectedRtp && _srtpClient->isConnected());

    switch (state)
    {
    case ice::IceSession::State::CONNECTED:
    {
        logger::info("ICE CONNECTED %s", _loggableId.c_str(), _isConnected ? "complete" : "");

        const auto candidatePair = _rtpIceSession->getSelectedPair();
        const auto rtt = _rtpIceSession->getSelectedPairRtt();
        _rttNtp = utils::Time::absToNtp32(rtt);

        logger::info("Pair selected: %s - %s  rtt:%" PRIu64 "us",
            _loggableId.c_str(),
            candidatePair.first.address.toFixedString().c_str(),
            maybeMasked(candidatePair.second.address).c_str(),
            rtt / utils::Time::us);

        if (_selectedRtp)
        {
            logger::info("candidate selected %s %s, %s",
                _loggableId.c_str(),
                _peerRtpPort.getFamilyString().c_str(),
                ice::toString(_selectedRtp->getTransportType()).c_str(),
                ice::toString(candidatePair.second.type).c_str());

            if (_srtpClient->getState() == SrtpClient::State::READY)
            {
                DtlsTimerJob::start(_jobQueue, *this, *_srtpClient, 1);
            }
        }

        if (_rtpIceSession->getRole() == ice::IceRole::CONTROLLING)
        {
            for (auto& endpoint : _rtpEndpoints)
            {
                if (endpoint.get() != _selectedRtp && endpoint->getTransportType() == ice::TransportType::TCP)
                {
                    logger::info("discarding %s, ref %ld",
                        _loggableId.c_str(),
                        endpoint->getName(),
                        endpoint.use_count());

                    _rtpIceSession->onTcpRemoved(endpoint.get());
                    endpoint->unregisterListener(this);
                    ++_jobCounter;
                    endpoint->stop(this);
                }
            }
        }

        if (isConnected())
        {
            onTransportConnected();
        }

        break;
    }
    case ice::IceSession::State::FAILED:
        logger::error("ICE FAILED", _loggableId.c_str());
        break;

    case ice::IceSession::State::IDLE:
        logger::info("ICE IDLE", _loggableId.c_str());
        break;

    case ice::IceSession::State::GATHERING:
        logger::info("ICE GATHERING", _loggableId.c_str());
        break;

    case ice::IceSession::State::CONNECTING:
        logger::info("ICE CONNECTING", _loggableId.c_str());
        break;

    case ice::IceSession::State::READY:
        logger::info("ICE GATHER READY", _loggableId.c_str());
        break;
    default:
        break;
    }
}

void TransportImpl::onTransportConnected()
{
    auto* dataReceiver = _dataReceiver.load();
    if (dataReceiver)
    {
        dataReceiver->onConnected(this);
    }
    if (_remoteSctpPort.isSet() && _srtpClient->isDtlsClient())
    {
        doConnectSctp();
    }
}

void TransportImpl::setDataReceiver(DataReceiver* dataReceiver)
{
    _dataReceiver.store(dataReceiver);
}

void TransportImpl::onSrtpStateChange(SrtpClient*, const SrtpClient::State state)
{
    _dtlsState = state;
    _isConnected = (_selectedRtp && _srtpClient->isConnected());

    logger::info("SRTP %s, transport connected%u",
        getLoggableId().c_str(),
        api::utils::toString(state),
        _isConnected.load());
    if (state == SrtpClient::State::CONNECTED && isConnected())
    {
        onTransportConnected();
    }
    else if (state == SrtpClient::State::CONNECTING)
    {
        DtlsTimerJob::start(_jobQueue, *this, *_srtpClient, 1);
    }
    else if (state == SrtpClient::State::READY && _selectedRtp)
    {
        DtlsTimerJob::start(_jobQueue, *this, *_srtpClient, 1);
    }
}

int32_t TransportImpl::sendDtls(const char* buffer, const uint32_t length)
{
    assert(length >= 0);
    if (!buffer || length <= 0)
    {
        return 0;
    }

    if (!_selectedRtp)
    {
        logger::info("no rtp endpoint selected for DTLS", _loggableId.c_str());
        return length;
    }
    if (buffer[0] != DTLSContentType::applicationData)
    {
        logger::debug("sending DTLS protocol message, %dB", _loggableId.c_str(), length);
    }

    if (length > memory::Packet::size || length > _config.mtu)
    {
        logger::error("DTLS message %dB exceeds MTU %u", _loggableId.c_str(), length, _config.mtu.get());
        return 0;
    }

    auto packet = memory::makeUniquePacket(_mainAllocator, buffer, length);
    if (packet)
    {
        _selectedRtp->sendTo(_peerRtpPort, std::move(packet));
        return length;
    }
    else
    {
        logger::error("Failed to send DTLS. Packet pool depleted", _loggableId.c_str());
        return 0;
    }
}

void TransportImpl::setAudioPayloads(uint8_t payloadType,
    utils::Optional<uint8_t> telephoneEventPayloadType,
    uint32_t rtpFrequency)
{
    _audio.payloadType = payloadType;
    _audio.telephoneEventPayloadType = telephoneEventPayloadType;
    _audio.rtpFrequency = rtpFrequency;
}

/**
 * extension id = 0 means off
 */
void TransportImpl::setAbsSendTimeExtensionId(uint8_t extensionId)
{
    _absSendTimeExtensionId = extensionId;
}

uint16_t TransportImpl::allocateOutboundSctpStream()
{
    if (_sctpAssociation)
    {
        return _sctpAssociation->allocateStream();
    }
    else
    {
        logger::error("cannot allocate outbound sctp stream. No association established yet", _loggableId.c_str());
    }
    return 0xFFFFu;
}

bool TransportImpl::sendSctp(const uint16_t streamId,
    const uint32_t protocolId,
    const void* data,
    const uint16_t length)
{
    if (!_remoteSctpPort.isSet() || !_sctpAssociation)
    {
        logger::warn("SCTP not established yet.", _loggableId.c_str());
        return false;
    }

    if (length == 0 || 2 * sizeof(uint32_t) + length > memory::Packet::size)
    {
        logger::error("sctp message invalid size %u", getLoggableId().c_str(), length);
        return false;
    }

    _jobQueue
        .addJob<SctpSendJob>(*_sctpAssociation, streamId, protocolId, data, length, _mainAllocator, _jobQueue, *this);

    return true;
}

void TransportImpl::connectSctp()
{
    _jobQueue.post(_jobCounter, [this]() { doConnectSctp(); });
}

void TransportImpl::doConnectSctp()
{
    if (_srtpClient->getMode() != srtp::Mode::DTLS)
    {
        return;
    }

    if (_sctpAssociation)
    {
        logger::info("SCTP association already created", _loggableId.c_str());
        return;
    }

    _sctpAssociation = sctp::createSctpAssociation(_loggableId.getInstanceId(),
        *_sctpServerPort,
        _remoteSctpPort.get(),
        this,
        _sctpServerPort->getConfig());

    const auto timestamp = utils::Time::getAbsoluteTime();
    _sctpAssociation->connect(0xFFFFu, 0xFFFFu, timestamp);
    SctpTimerJob::start(_jobQueue, *this, *_sctpAssociation, _sctpAssociation->nextTimeout(timestamp));
}

bool TransportImpl::sendSctpPacket(const void* data, size_t length)
{
    _rateController.onSctpSent(utils::Time::getAbsoluteTime(), length);
    _srtpClient->sendApplicationData(data, length);
    return true;
}

bool TransportImpl::onSctpInitReceived(sctp::SctpServerPort* serverPort,
    const uint16_t srcPort,
    const sctp::SctpPacket& sctpPacket,
    const uint64_t timestamp,
    uint16_t& inboundStreams,
    uint16_t& outboundStreams)
{
    if (!_remoteSctpPort.isSet() || srcPort != _remoteSctpPort.get())
    {
        logger::warn("remote SCTP port mismatch %u -> %u", _loggableId.c_str(), srcPort, _remoteSctpPort.valueOr(0));
        return false;
    }

    if (_sctpAssociation)
    {
        logger::info("unexpected INIT received", _loggableId.c_str());
        _sctpAssociation->onPacketReceived(sctpPacket, timestamp);
        return false;
    }

    outboundStreams = 0xFFFF;
    inboundStreams = 0xFFFF;
    return true; // always accept connection for now
}

void TransportImpl::onSctpCookieEchoReceived(sctp::SctpServerPort* serverPort,
    const uint16_t srcPort,
    const sctp::SctpPacket& sctpPacket,
    const uint64_t timestamp)
{
    if (_sctpAssociation)
    {
        _sctpAssociation->onPacketReceived(sctpPacket, timestamp);
    }
    else
    {
        _sctpAssociation = sctp::createSctpAssociation(_loggableId.getInstanceId(),
            *serverPort,
            sctpPacket,
            this,
            serverPort->getConfig());
        _sctpAssociation->onCookieEcho(sctpPacket, timestamp);
    }
}

void TransportImpl::onSctpReceived(sctp::SctpServerPort* serverPort,
    const uint16_t srcPort,
    const sctp::SctpPacket& sctpPacket,
    const uint64_t timestamp)
{
    if (_sctpAssociation)
    {
        const auto currentTimeout = _sctpAssociation->nextTimeout(timestamp);
        const auto nextTimeout = _sctpAssociation->onPacketReceived(sctpPacket, timestamp);

        if (nextTimeout >= 0 && currentTimeout > nextTimeout)
        {
            SctpTimerJob::start(_jobQueue, *this, *_sctpAssociation, nextTimeout);
        }
    }
}

void TransportImpl::onSctpStateChanged(sctp::SctpAssociation* session, sctp::SctpAssociation::State state)
{
    logger::info("SCTP state %s", _loggableId.c_str(), sctp::toString(state));
}

void TransportImpl::onSctpFragmentReceived(sctp::SctpAssociation* session,
    const uint16_t streamId,
    const uint16_t streamSequenceNumber,
    const uint32_t payloadProtocol,
    const void* buffer,
    const size_t length,
    const uint64_t timestamp)
{
    auto* dataReceiver = _dataReceiver.load();
    if (dataReceiver)
    {
        dataReceiver->onSctpMessage(this, streamId, streamSequenceNumber, payloadProtocol, buffer, length);
    }
}
void TransportImpl::onSctpEstablished(sctp::SctpAssociation* session)
{
    auto dataReceiver = _dataReceiver.load();
    if (dataReceiver)
    {
        dataReceiver->onSctpEstablished(this);
    }
}
void TransportImpl::onSctpClosed(sctp::SctpAssociation* session)
{
    logger::info("SCTP association closed by peer", _loggableId.c_str());
    _sctpAssociation->close();
}

void TransportImpl::onSctpChunkDropped(sctp::SctpAssociation* session, size_t size)
{
    logger::error("Sctp chunk dropped due to overload or unknown type", _loggableId.c_str());
}

uint64_t TransportImpl::getRtt() const
{
    return (static_cast<uint64_t>(_rttNtp) * utils::Time::sec) >> 16;
}

uint64_t TransportImpl::getInboundPacketCount() const
{
    return _inboundMetrics.packetCount.load();
}

void TransportImpl::setRtxProbeSource(const uint32_t ssrc, uint32_t* sequenceCounter, const uint16_t payloadType)
{
    _rtxProbeSsrc = ssrc;
    _rtxProbeSequenceCounter = sequenceCounter;
    _videoRtxPayloadType = payloadType;
    _rateController.setRtpProbingEnabled(!!sequenceCounter);
}

void TransportImpl::runTick(uint64_t timestamp)
{
    if (_pacingInUse.load())
    {
        _jobQueue.addJob<RunTickJob>(*this, timestamp);
    }
}

void TransportImpl::doRunTick(const uint64_t timestamp)
{
    _lastTickJobStartTimestamp = timestamp;

    auto drainMode = _uplinkEstimationEnabled && _config.rctl.useUplinkEstimate ? DrainPacingBufferMode::UseBudget
                                                                                : DrainPacingBufferMode::DrainAll;

    drainPacingBuffer(timestamp, drainMode);
    if (DrainPacingBufferMode::UseBudget == drainMode)
    {
        sendPadding(timestamp);
    }
    _pacingInUse = !_pacingQueue.empty() || !_rtxPacingQueue.empty();
    _pacingQueueStats.pacingQueueSize = _pacingQueue.size();
    _pacingQueueStats.rtxPacingQueueSize = _rtxPacingQueue.size();
}

memory::UniquePacket TransportImpl::tryFetchPriorityPacket(size_t budget)
{
    auto& queue = _rtxPacingQueue.empty() ? _pacingQueue : _rtxPacingQueue;
    return !queue.empty() && budget >= queue.back()->getLength() + _config.ipOverhead ? queue.fetchBack() : nullptr;
}

void TransportImpl::drainPacingBuffer(uint64_t timestamp, DrainPacingBufferMode mode)
{
    auto budget = DrainPacingBufferMode::UseBudget == mode ? _rateController.getPacingBudget(timestamp) : SIZE_MAX;
    while (auto packet = tryFetchPriorityPacket(budget))
    {
        budget -= packet->getLength() + _config.ipOverhead;
        protectAndSendRtp(timestamp, std::move(packet));
    }
}

void TransportImpl::setTag(const char* tag)
{
    utils::strncpy(_tag, tag, sizeof(_tag));
}

void TransportImpl::onIceCandidateAccepted(ice::IceSession* session,
    ice::IceEndpoint* endpoint,
    const ice::IceCandidate& remoteCandidate)
{
    logger::info("candidate accepted: %s %s, %s - %s",
        _loggableId.c_str(),
        endpoint->getLocalPort().getFamilyString().c_str(),
        ice::toString(endpoint->getTransportType()).c_str(),
        endpoint->getLocalPort().toFixedString().c_str(),
        maybeMasked(remoteCandidate.address).c_str());

    for (auto& udpEndpoint : _rtpEndpoints)
    {
        // static cast for type safe raw pointer comparison
        if (endpoint == static_cast<ice::IceEndpoint*>(udpEndpoint.get()))
        {
            udpEndpoint->registerListener(remoteCandidate.address, this);
            return;
        }
    }

    for (auto& udpEndpoint : _rtpEndpoints)
    {
        if (endpoint == static_cast<ice::IceEndpoint*>(udpEndpoint.get()))
        {
            udpEndpoint->registerListener(remoteCandidate.address, this);
            return;
        }
    }

    logger::error("Not possible to register listener as the endpoint was not found: %s %s, %s - %s",
        _loggableId.c_str(),
        endpoint->getLocalPort().getFamilyString().c_str(),
        ice::toString(endpoint->getTransportType()).c_str(),
        endpoint->getLocalPort().toString().c_str(),
        remoteCandidate.address.toString().c_str());
}

void TransportImpl::onIceDiscardCandidate(ice::IceSession* session,
    ice::IceEndpoint* endpoint,
    const transport::SocketAddress& sourcePort)
{
    if (session->getState() <= ice::IceSession::State::CONNECTED)
    {
        logger::info("candidate discarded: %s %s, %s - %s",
            _loggableId.c_str(),
            endpoint->getLocalPort().getFamilyString().c_str(),
            ice::toString(endpoint->getTransportType()).c_str(),
            endpoint->getLocalPort().toFixedString().c_str(),
            maybeMasked(sourcePort).c_str());
    }

    for (auto& udpEndpoint : _rtpEndpoints)
    {
        // static cast for type safe raw pointer comparison
        if (endpoint == static_cast<ice::IceEndpoint*>(udpEndpoint.get()))
        {
            udpEndpoint->unregisterListener(sourcePort, this);
            return;
        }
    }

    for (auto& udpEndpoint : _rtcpEndpoints)
    {
        if (endpoint == static_cast<ice::IceEndpoint*>(udpEndpoint.get()))
        {
            udpEndpoint->unregisterListener(sourcePort, this);
            return;
        }
    }
}

void TransportImpl::getSdesKeys(std::vector<srtp::AesKey>& sdesKeys) const
{
    srtp::AesKey key;
    for (uint32_t profile = 1; profile < srtp::PROFILE_LAST; ++profile)
    {
        _srtpClient->getLocalKey(static_cast<srtp::Profile>(profile), key);
        if (key.getLength() > 0)
        {
            sdesKeys.push_back(key);
        }
    }
}

void TransportImpl::asyncSetRemoteSdesKey(const srtp::AesKey& key)
{
    _jobQueue.post(_jobCounter, [this, key]() { _srtpClient->setRemoteKey(key); });
}

} // namespace transport
