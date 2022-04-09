#include "TransportImpl.h"
#include "bwe/BandwidthEstimator.h"
#include "config/Config.h"
#include "dtls/SrtpClient.h"
#include "dtls/SrtpClientFactory.h"
#include "dtls/SslDtls.h"
#include "ice/IceSession.h"
#include "logger/Logger.h"
#include "logger/PacketLogger.h"
#include "rtp/RtcpFeedback.h"
#include "rtp/RtpHeader.h"
#include "sctp/SctpAssociation.h"
#include "transport/DtlsJob.h"
#include "transport/IceJob.h"
#include "transport/SctpJob.h"
#include "utils/SocketAddress.h"
#include "utils/StdExtensions.h"
#include <arpa/inet.h>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace transport
{

constexpr uint32_t rtxPayloadType = 96;

// we have to serialize operations on srtp client
// timers, start and receive must be done from same serialized jobmanager.
class PacketReceiveJob : public jobmanager::Job
{
public:
    PacketReceiveJob(TransportImpl& transport,
        Endpoint& endpoint,
        const SocketAddress& source,
        memory::UniquePacket packet,
        void (TransportImpl::*receiveMethod)(Endpoint& endpoint,
            const SocketAddress& source,
            memory::UniquePacket packet,
            uint64_t timestamp))
        : _transport(transport),
          _endpoint(endpoint),
          _packet(std::move(packet)),
          _source(source),
          _timestamp(utils::Time::getAbsoluteTime()),
          _receiveMethod(receiveMethod)
    {
    }

    ~PacketReceiveJob() {}

    void run() override
    {
        DBGCHECK_SINGLETHREADED(_transport._singleThreadMutex);

        (_transport.*_receiveMethod)(_endpoint, _source, std::move(_packet), _timestamp);
    }

private:
    TransportImpl& _transport;
    Endpoint& _endpoint;
    memory::UniquePacket _packet;
    const SocketAddress _source;
    uint64_t _timestamp;

    void (TransportImpl::*_receiveMethod)(Endpoint&, const SocketAddress&, memory::UniquePacket packet, uint64_t);
};

class IceDisconnectJob : public jobmanager::Job
{
public:
    IceDisconnectJob(TransportImpl& transport, Endpoint& endpoint) : _transport(transport), _endpoint(endpoint) {}

    void run() override { _transport.onIceDisconnect(_endpoint); }

private:
    TransportImpl& _transport;
    Endpoint& _endpoint;
};

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
        memory::PacketPoolAllocator& allocator)
        : CountedJob(transport.getJobCounter()),
          _transport(transport),
          _allocator(allocator),
          _iceCredentials(nullptr),
          _iceCandidates{0}
    {
        _iceCredentials = memory::makePacket(allocator);
        if (!_iceCredentials)
        {
            logger::error("failed to allocate packet", "setRemoteIce");
            return;
        }
        auto& iceSettings = *reinterpret_cast<IceCredentials*>(_iceCredentials->get());
        utils::strncpy(iceSettings.ufrag, credentials.first.c_str(), sizeof(iceSettings.ufrag));
        utils::strncpy(iceSettings.password, credentials.second.c_str(), sizeof(iceSettings.password));

        memory::Packet* icePacket = nullptr;
        for (size_t i = 0; i < candidates.size() && i < maxCandidatePackets * IceCandidates::maxCandidateCount; ++i)
        {
            if (i % IceCandidates::maxCandidateCount == 0)
            {
                icePacket = memory::makePacket(allocator);
                if (!icePacket)
                {
                    logger::error("failed to allocate packet", "setRemoteIce");
                    return;
                }
                _iceCandidates[i / maxCandidatePackets] = icePacket;
                auto& iceCandidates = *reinterpret_cast<IceCandidates*>(icePacket->get());
                iceCandidates.candidateCount = 0;
            }
            auto& iceCandidates = *reinterpret_cast<IceCandidates*>(icePacket->get());
            iceCandidates.candidates[i % IceCandidates::maxCandidateCount] = candidates[i];
            iceCandidates.candidateCount++;
        }
    }

    void run() override
    {
        if (!_iceCredentials)
        {
            return;
        }

        _transport.doSetRemoteIce(_iceCredentials, _iceCandidates);
    }

    ~IceSetRemoteJob()
    {
        if (_iceCredentials)
        {
            _allocator.free(_iceCredentials);
        }

        for (size_t i = 0; i < maxCandidatePackets; ++i)
        {
            if (!_iceCandidates[i])
            {
                break;
            }
            _allocator.free(_iceCandidates[i]);
        }
    }

private:
    TransportImpl& _transport;
    memory::PacketPoolAllocator& _allocator;
    memory::Packet* _iceCredentials;
    memory::Packet* _iceCandidates[maxCandidatePackets + 1];
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

// must be serialized after set Ice and set Dtls
// otherwise transport will not know when we are connected and may start connect too early
class ConnectJob : public jobmanager::CountedJob
{
public:
    ConnectJob(TransportImpl& transport) : CountedJob(transport.getJobCounter()), _transport(transport) {}

    void run() override { _transport.doConnect(); }

private:
    TransportImpl& _transport;
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
          _packet(memory::makePacket(allocator)),
          _allocator(allocator),
          _transport(transport)
    {
        if (_packet)
        {
            if (sizeof(SctpDataChunk) + length > memory::Packet::size)
            {
                logger::error("sctp message too big %u", _transport.getLoggableId().c_str(), length);
                allocator.free(_packet);
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

    ~SctpSendJob()
    {
        if (_packet)
        {
            _allocator.free(_packet);
        }
    }

    void run() override
    {
        if (!_packet)
        {
            return;
        }

        auto timestamp = utils::Time::getAbsoluteTime();
        auto current = _sctpAssociation.nextTimeout(timestamp);
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
        if (nextTimeout >= 0 && (current < 0 || current > nextTimeout))
        {
            SctpTimerJob::start(_jobQueue, _transport, _sctpAssociation, nextTimeout);
        }
    }

private:
    jobmanager::JobQueue& _jobQueue;
    sctp::SctpAssociation& _sctpAssociation;
    memory::Packet* _packet;
    memory::PacketPoolAllocator& _allocator;
    TransportImpl& _transport;
};

class ConnectSctpJob : public jobmanager::CountedJob
{
public:
    ConnectSctpJob(TransportImpl& transport) : CountedJob(transport.getJobCounter()), _transport(transport) {}

    void run() override { _transport.doConnectSctp(); }

private:
    TransportImpl& _transport;
};

// Placed last in queue during shutdown to reduce ref count when all jobs are complete.
// This means other jobs in the transport job queue do not have to have counters
class ShutdownJob : public jobmanager::CountedJob
{
public:
    explicit ShutdownJob(std::atomic_uint32_t& ownerCount) : CountedJob(ownerCount) {}

    void run() override {}
};

class RunTickJob : public jobmanager::CountedJob
{
public:
    RunTickJob(TransportImpl& transport) : CountedJob(transport.getJobCounter()), _transport(transport) {}

    void run() override { _transport.doRunTick(utils::Time::getAbsoluteTime()); }

private:
    TransportImpl& _transport;
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
    const std::vector<Endpoint*>& rtpEndPoints,
    const std::vector<ServerEndpoint*>& tcpEndpoints,
    TcpEndpointFactory* tcpEndpointFactory,
    memory::PacketPoolAllocator& allocator)
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
        allocator);
}

std::shared_ptr<RtcTransport> createTransport(jobmanager::JobManager& jobmanager,
    SrtpClientFactory& srtpClientFactory,
    const size_t endpointIdHash,
    const config::Config& config,
    const sctp::SctpConfig& sctpConfig,
    const bwe::Config& bweConfig,
    const bwe::RateControllerConfig& rateControllerConfig,
    const std::vector<Endpoint*>& rtpEndPoints,
    const std::vector<Endpoint*>& rtcpEndPoints,
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
    const std::vector<Endpoint*>& rtpEndPoints,
    const std::vector<Endpoint*>& rtcpEndPoints,
    memory::PacketPoolAllocator& allocator)
    : _isInitialized(false),
      _loggableId("Transport"),
      _endpointIdHash(endpointIdHash),
      _config(config),
      _srtpClient(srtpClientFactory.create(this)),
      _dtlsEnabled(false),
      _callbackRefCount(0),
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
      _outboundSsrcCounters(256),
      _inboundSsrcCounters(16),
      _isRunning(true),
      _absSendTimeExtensionId(0),
      _sctpConfig(sctpConfig),
      _bwe(std::make_unique<bwe::BandwidthEstimator>(bweConfig)),
      _rateController(_loggableId.getInstanceId(), rateControllerConfig),
      _rtxProbeSsrc(0),
      _rtxProbeSequenceCounter(nullptr),
      _pacingInUse(false)
{
    assert(endpointIdHash != 0);

    logger::info("SRTP client: %s", _loggableId.c_str(), _srtpClient->getLoggableId().c_str());
    assert(_srtpClient->isInitialized());
    if (!_srtpClient->isInitialized())
    {
        return;
    }
    _srtpClient->setSslWriteBioListener(this);

    for (auto* endpoint : rtpEndPoints)
    {
        _rtpEndpoints.push_back(endpoint);
        endpoint->registerDefaultListener(this);
        ++_callbackRefCount;
    }
    for (auto* endpoint : rtcpEndPoints)
    {
        _rtcpEndpoints.push_back(endpoint);
        endpoint->registerDefaultListener(this);
        ++_callbackRefCount;
    }
    if (_callbackRefCount > 0)
    {
        _jobCounter = 1;
    }

    if (_rtpEndpoints.size() == 1)
    {
        _selectedRtp = _rtpEndpoints.front();
    }
    if (_rtcpEndpoints.size() == 1)
    {
        _selectedRtcp = _rtcpEndpoints.front();
    }

    _isInitialized = true;
    logger::debug("started with %zu rtp + %zu rtcp endpoints. refcount %u",
        _loggableId.c_str(),
        _rtpEndpoints.size(),
        _rtcpEndpoints.size(),
        _callbackRefCount.load());
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
    const std::vector<Endpoint*>& sharedEndpoints,
    const std::vector<ServerEndpoint*>& tcpEndpoints,
    TcpEndpointFactory* tcpEndpointFactory,
    memory::PacketPoolAllocator& allocator)
    : _isInitialized(false),
      _loggableId("Transport"),
      _endpointIdHash(endpointIdHash),
      _config(config),
      _srtpClient(srtpClientFactory.create(this)),
      _dtlsEnabled(false),
      _tcpEndpointFactory(tcpEndpointFactory),
      _callbackRefCount(0),
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
      _outboundSsrcCounters(256),
      _inboundSsrcCounters(16),
      _isRunning(true),
      _absSendTimeExtensionId(0),
      _sctpConfig(sctpConfig),
      _bwe(std::make_unique<bwe::BandwidthEstimator>(bweConfig)),
      _rateController(_loggableId.getInstanceId(), rateControllerConfig),
      _rtxProbeSsrc(0),
      _rtxProbeSequenceCounter(nullptr),
      _pacingInUse(false)
{
    assert(endpointIdHash != 0);

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
        _rtpIceSession->attachLocalEndpoint(endpoint);
        endpoint->registerListener(_rtpIceSession->getLocalCredentials().first, this);
        ++_callbackRefCount;

        if (!iceConfig.publicIpv4.empty() && endpoint->getLocalPort().getFamily() == AF_INET)
        {
            auto addressPort = iceConfig.publicIpv4;
            addressPort.setPort(endpoint->getLocalPort().getPort());
            _rtpIceSession->addLocalCandidate(addressPort, endpoint);
        }
        if (!iceConfig.publicIpv6.empty() && endpoint->getLocalPort().getFamily() == AF_INET6)
        {
            auto addressPort = iceConfig.publicIpv6;
            addressPort.setPort(endpoint->getLocalPort().getPort());
            _rtpIceSession->addLocalCandidate(addressPort, endpoint);
        }
    }
    int index = 0;
    for (auto* endpoint : tcpEndpoints)
    {
        _rtpIceSession->addLocalTcpCandidate(ice::IceCandidate::Type::HOST,
            index++,
            endpoint->getLocalPort(),
            endpoint->getLocalPort(),
            ice::TcpType::PASSIVE);
        _tcpServerEndpoints.push_back(endpoint);
        endpoint->registerListener(_rtpIceSession->getLocalCredentials().first, this);
        ++_callbackRefCount;

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
    if (_callbackRefCount > 0)
    {
        _jobCounter = 1;
    }

    _rtpIceSession->gatherLocalCandidates(iceConfig.gather.stunServers, utils::Time::getAbsoluteTime());

    _isInitialized = true;
    logger::debug("started with %zu udp + %zu tcp endpoints. refcount %u",
        _loggableId.c_str(),
        _rtpEndpoints.size(),
        _tcpServerEndpoints.size(),
        _callbackRefCount.load());

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
        if (!endpoint->isShared())
        {
            endpoint->start();
        }
    }
    for (auto& endpoint : _rtcpEndpoints)
    {
        if (!endpoint->isShared())
        {
            endpoint->start();
        }
    }
    return true;
}

TransportImpl::~TransportImpl()
{
    if (_jobCounter.load() > 0 || _isRunning || _jobQueue.getCount() > 0)
    {
        logger::warn("~TransportImpl not idle %p running%u jobcount %u cbrefs %u, serialjobs %zu",
            _loggableId.c_str(),
            this,
            _isRunning.load(),
            _jobCounter.load(),
            _callbackRefCount.load(),
            _jobQueue.getCount());
    }

    for (auto* endpoint : _rtpEndpoints)
    {
        if (!endpoint->isShared())
        {
            delete endpoint;
        }
    }
    for (auto* endpoint : _rtcpEndpoints)
    {
        if (!endpoint->isShared())
        {
            delete endpoint;
        }
    }

    while (!_pacingQueue.empty())
    {
        _pacingQueue.pop_back();
    }
    while (!_rtxPacingQueue.empty())
    {
        _rtxPacingQueue.pop_back();
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

    if (_rtpIceSession)
    {
        _rtpIceSession->stop();
    }

    if (_sctpAssociation)
    {
        _sctpAssociation->close();
    }

    for (auto* endpoint : _tcpServerEndpoints)
    {
        endpoint->unregisterListener(_rtpIceSession->getLocalCredentials().first, this);
    }

    // there may be new tcp endpoints being created due to setRemoteIce candidates jobs
    for (const uint64_t waitStart = utils::Time::getAbsoluteTime(); _jobCounter.load() > 1 &&
         utils::Time::diffLE(waitStart, utils::Time::getAbsoluteTime(), 100 * utils::Time::ms);)
    {
        utils::Time::nanoSleep(10 * utils::Time::ms);
    }

    for (auto* ep : _rtpEndpoints)
    {
        if (!ep->isShared())
        {
            ep->registerDefaultListener(this);
            ep->closePort();
        }
        else
        {
            ep->unregisterListener(this);
        }
    }
    for (auto* ep : _rtcpEndpoints)
    {
        if (!ep->isShared())
        {
            ep->registerDefaultListener(this);
            ep->closePort();
        }
        else
        {
            ep->unregisterListener(this);
        }
    }

    _jobQueue.getJobManager().abortTimedJobs(getId());
    _isRunning = false;
}

void TransportImpl::onUnregistered(Endpoint& endpoint)
{
    logger::debug("unregistered %s, %d, %p", _loggableId.c_str(), endpoint.getName(), _callbackRefCount.load(), this);
    if (_callbackRefCount.fetch_sub(1) == 1)
    {
        // put a job last in queue to reduce owner count when all jobs are complete
        _jobQueue.addJob<ShutdownJob>(_jobCounter);
        _jobQueue.getJobManager().abortTimedJobs(getId());
        logger::debug("transport events stopped jobcount %u", _loggableId.c_str(), _jobCounter.load() - 1);
        _isInitialized = false;
        --_jobCounter;
    }
}

void TransportImpl::onServerPortUnregistered(ServerEndpoint& endpoint)
{
    logger::debug("unregistered %s, %d, %p", _loggableId.c_str(), endpoint.getName(), _callbackRefCount.load(), this);
    if (_callbackRefCount.fetch_sub(1) == 1)
    {
        // put a job last in queue to reduce owner count when all jobs are complete
        _jobQueue.addJob<ShutdownJob>(_jobCounter);
        logger::debug("transport events stopped %u", _loggableId.c_str(), _jobCounter.load() - 1);
        _isInitialized = false;
        --_jobCounter;
    }
}

void TransportImpl::onRtpReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet)
{
    if (!_jobQueue
             .addJob<PacketReceiveJob>(*this, endpoint, source, std::move(packet), &TransportImpl::internalRtpReceived))
    {
        logger::error("job queue full RTCP", _loggableId.c_str());
    }
}

void TransportImpl::internalRtpReceived(Endpoint& endpoint,
    const SocketAddress& source,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    ++_inboundMetrics.packetCount;
    _inboundMetrics.bytesCount += packet->getLength();

    if (!_srtpClient->isDtlsConnected())
    {
        logger::debug("RTP received, dtls not connected yet", _loggableId.c_str());
        return;
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

    if (utils::Time::diffGE(_rtcp.lastSendTime, timestamp, utils::Time::ms * 100) || rembReady)
    {
        sendReports(timestamp, rembReady);
    }

    const uint32_t ssrc = rtpHeader->ssrc;
    auto& ssrcState = getInboundSsrc(ssrc); // will do nothing if already exists
    ssrcState.onRtpReceived(*packet, timestamp);

    if (ssrcState.currentRtpSource != source)
    {
        logger::debug("RTP ssrc %u from %s", _loggableId.c_str(), ssrc, source.toString().c_str());
        ssrcState.currentRtpSource = source;
    }

    const uint32_t extendedSequenceNumber = ssrcState.getExtendedSequenceNumber() -
        static_cast<int16_t>(
            static_cast<uint16_t>(ssrcState.getExtendedSequenceNumber() & 0xFFFFu) - rtpHeader->sequenceNumber.get());

    dataReceiver->onRtpPacketReceived(this, std::move(packet), extendedSequenceNumber, timestamp);
}

void TransportImpl::onDtlsReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet)
{
    if (!_jobQueue.addJob<PacketReceiveJob>(*this,
            endpoint,
            source,
            std::move(packet),
            &TransportImpl::internalDtlsReceived))
    {
        logger::warn("job queue full DTLS", _loggableId.c_str());
    }
}

void TransportImpl::internalDtlsReceived(Endpoint& endpoint,
    const SocketAddress& source,
    memory::UniquePacket packet,
    uint64_t timestamp)
{
    ++_inboundMetrics.packetCount;
    _inboundMetrics.bytesCount += packet->getLength();
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
        logger::debug("received DTLS protocol message from %s, %zu",
            _loggableId.c_str(),
            source.toString().c_str(),
            packet->getLength());
        _srtpClient->onMessageReceived(reinterpret_cast<const char*>(packet->get()), packet->getLength());
    }
}

void TransportImpl::onRtcpReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet)
{
    if (!_jobQueue.addJob<PacketReceiveJob>(*this,
            endpoint,
            source,
            std::move(packet),
            &TransportImpl::internalRtcpReceived))
    {
        logger::warn("job queue full RTCP", _loggableId.c_str());
    }
}

void TransportImpl::internalRtcpReceived(Endpoint& endpoint,
    const SocketAddress& source,
    memory::UniquePacket packet,
    uint64_t timestamp)
{
    ++_inboundMetrics.packetCount;
    _inboundMetrics.bytesCount += packet->getLength();
    if (!_srtpClient->isDtlsConnected())
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

    const auto now = std::chrono::system_clock::now();
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
    memory::UniquePacket packet)
{
    if (_rtpIceSession)
    {
        if (ice::isResponse(packet->get()) && !_rtpIceSession->isResponseAuthentic(packet->get(), packet->getLength()))
        {
            return;
        }

        if (_rtpIceSession->isRequestAuthentic(packet->get(), packet->getLength()) ||
            _rtpIceSession->isResponseAuthentic(packet->get(), packet->getLength()))
        {
            endpoint.registerListener(source, this);
        }
    }

    if (!_jobQueue
             .addJob<PacketReceiveJob>(*this, endpoint, source, std::move(packet), &TransportImpl::internalIceReceived))
    {
        logger::error("job queue full ICE", _loggableId.c_str());
    }
}

void TransportImpl::internalIceReceived(Endpoint& endpoint,
    const SocketAddress& source,
    memory::UniquePacket packet,
    uint64_t timestamp)
{
    ++_inboundMetrics.packetCount;
    _inboundMetrics.bytesCount += packet->getLength();

    const auto msg = ice::StunMessage::fromPtr(packet->get());
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
    if (_rtpIceSession && _rtpIceSession->isAttached(&endpoint))
    {
        _rtpIceSession->onPacketReceived(&endpoint, source, packet->get(), packet->getLength(), timestamp);
    }
    else if (_rtpIceSession && endpoint.getTransportType() == ice::TransportType::TCP)
    {
        logger::debug("new tcp connection %s received %zu",
            _loggableId.c_str(),
            endpoint.getName(),
            packet->getLength());

        endpoint.registerDefaultListener(this);
        ++_callbackRefCount;
        _rtpEndpoints.push_back(&endpoint);
        _rtpIceSession->onPacketReceived(&endpoint, source, packet->get(), packet->getLength(), timestamp);
    }
}

void TransportImpl::onIceDisconnect(Endpoint& endpoint)
{
    _rtpIceSession->onTcpDisconnect(&endpoint);
}

void TransportImpl::onPortClosed(Endpoint& endpoint)
{
    if (endpoint.getTransportType() == ice::TransportType::TCP)
    {
        _jobQueue.addJob<IceDisconnectJob>(*this, endpoint);
    }
    endpoint.unregisterListener(this);
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

        if (_config.rctl.enable)
        {
            _outboundMetrics.estimatedKbps = _rateController.getTargetRate();
        }

        auto& ssrcState = getInboundSsrc(senderReport->ssrc);
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
        if (_config.rctl.enable)
        {
            _outboundMetrics.estimatedKbps = _rateController.getTargetRate();
        }
    }
    else if (rtp::isRemb(&header))
    {
        const auto& remb = reinterpret_cast<const rtp::RtcpRembFeedback&>(header);
        const uint32_t estimatedKbps = remb.getBitrate() / 1000LLU;
        _outboundRembEstimateKbps = estimatedKbps;
        if (!_config.rctl.enable)
        {
            _outboundMetrics.estimatedKbps = estimatedKbps;
        }
    }

    if (rttNtp < ~0u)
    {
        _rttNtp = rttNtp;
    }
}

RtpReceiveState& TransportImpl::getInboundSsrc(const uint32_t ssrc)
{
    auto ssrcIt = _inboundSsrcCounters.find(ssrc);
    if (ssrcIt != _inboundSsrcCounters.cend())
    {
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

        auto pairIt = _inboundSsrcCounters.emplace(ssrc, _config);
        return pairIt.first->second;
    }
    else
    {
        auto pairIt = _inboundSsrcCounters.emplace(ssrc, _config);
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
    return _pacingQueue.size();
}

uint32_t TransportImpl::getRtxPacingQueueCount() const
{
    return _rtxPacingQueue.size();
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
    if (!_config.rctl.enable || !_rtxProbeSequenceCounter)
    {
        return;
    }

    auto budget = _rateController.getPacingBudget(timestamp);
    auto* pacingQueue = _rtxPacingQueue.empty() ? &_pacingQueue : &_rtxPacingQueue;
    while (!pacingQueue->empty() && budget >= pacingQueue->back()->getLength() + _config.ipOverhead)
    {
        auto packet = std::move(pacingQueue->back());
        pacingQueue->pop_back();
        budget -= std::min(budget, packet->getLength() + _config.ipOverhead);

        protectAndSendRtp(timestamp, std::move(packet));
        pacingQueue = _rtxPacingQueue.empty() ? &_pacingQueue : &_rtxPacingQueue;
    }

    uint16_t padding = 0;
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
            padRtpHeader->payloadType = rtxPayloadType;
            padRtpHeader->timestamp = 1293887;
            padRtpHeader->sequenceNumber = (*_rtxProbeSequenceCounter)++ & 0xFFFF;
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

void TransportImpl::sendRtcpPadding(uint64_t timestamp, uint32_t ssrc, uint16_t nextPacketSize)
{
    if (!_config.rctl.enable || _rtxProbeSequenceCounter)
    {
        return;
    }

    uint16_t padding = 0;
    const auto paddingCount = _rateController.getPadding(timestamp, nextPacketSize, padding);
    for (uint32_t i = 0; i < paddingCount && _selectedRtcp; ++i)
    {
        auto padPacket = memory::makeUniquePacket(_mainAllocator);
        if (padPacket)
        {
            auto* rtcpPadding = rtp::RtcpApplicationSpecific::create(padPacket->get(), ssrc, "BRPP", padding);
            padPacket->setLength(rtcpPadding->header.size());
            _rateController.onRtcpPaddingSent(timestamp, ssrc, padPacket->getLength());
            doProtectAndSend(timestamp, std::move(padPacket), _peerRtcpPort, _selectedRtcp);
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
        while (!pacingQueue.empty())
        {
            pacingQueue.pop_back();
        }
    }
}
} // namespace
void TransportImpl::protectAndSend(memory::UniquePacket packet)
{
    DBGCHECK_SINGLETHREADED(_singleThreadMutex);

    assert(_srtpClient);
    const auto timestamp = utils::Time::getAbsoluteTime();

    if (!_srtpClient || !_selectedRtp || !isConnected())
    {
        return;
    }

    if (rtp::isRtpPacket(*packet))
    {
        sendReports(timestamp);
        const auto* rtpHeader = rtp::RtpHeader::fromPacket(*packet);
        const auto payloadType = rtpHeader->payloadType;
        const auto isAudio = (payloadType <= 8 || payloadType == _audio.payloadType);

        clearPacingQueueIfFull(_pacingQueue);
        clearPacingQueueIfFull(_rtxPacingQueue);
        if (payloadType == rtxPayloadType)
        {
            _rtxPacingQueue.push_front(std::move(packet));
        }
        else if (!isAudio)
        {
            _pacingQueue.push_front(std::move(packet));
        }
        else
        {
            sendRtcpPadding(timestamp, rtpHeader->ssrc, packet->getLength());
            protectAndSendRtp(timestamp, std::move(packet));
        }

        doRunTick(timestamp);
    }
    else if (_selectedRtcp)
    {
        sendRtcp(std::move(packet), timestamp);
    }
}

void TransportImpl::protectAndSendRtp(uint64_t timestamp, memory::UniquePacket packet)
{
    const auto* rtpHeader = rtp::RtpHeader::fromPacket(*packet);
    const auto payloadType = rtpHeader->payloadType;
    const auto isAudio = (payloadType <= 8 || payloadType == _audio.payloadType);
    const uint32_t rtpFrequency = isAudio ? _audio.rtpFrequency : 90000;

    if (_absSendTimeExtensionId)
    {
        rtp::setTransmissionTimestamp(*packet, _absSendTimeExtensionId, timestamp);
    }

    auto& ssrcState = getOutboundSsrc(rtpHeader->ssrc, rtpFrequency);
    if (static_cast<int16_t>(rtpHeader->sequenceNumber.get() - (ssrcState.getSentSequenceNumber() & 0xFFFFu)) < 1)
    {
        logger::info("out of order transmission ssrc %u, seqno %u, last %u",
            _loggableId.c_str(),
            rtpHeader->ssrc.get(),
            rtpHeader->sequenceNumber.get(),
            ssrcState.getSentSequenceNumber() & 0xFFFFu);
    }

    ssrcState.onRtpSent(timestamp, *packet);
    _rateController.onRtpSent(timestamp, rtpHeader->ssrc, rtpHeader->sequenceNumber, packet->getLength());
    doProtectAndSend(timestamp, std::move(packet), _peerRtpPort, _selectedRtp);
}

// Send sender reports and receiver reports as needed for the inbound and outbound ssrcs.
// each sender report send time is offset 1/65536 sec to make them unique when we look them up
// after receive blocks arrive as they reference the SR by ntp timestamp
void TransportImpl::sendReports(uint64_t timestamp, bool rembReady)
{
    const int MINIMUM_SR = 7 * sizeof(uint32_t);
    const int MINIMUM_RR = 2 * sizeof(uint32_t);

    uint32_t senderReportCount = 0;
    uint32_t senderReportSsrcs[_outboundSsrcCounters.capacity()];
    for (auto& it : _outboundSsrcCounters)
    {
        const auto remainingTime = it.second.timeToSenderReport(timestamp);
        if (remainingTime == 0)
        {
            senderReportSsrcs[senderReportCount++] = it.first;
        }
    }

    uint32_t receiverReportCount = 0;
    uint32_t receiverReportSsrcs[_inboundSsrcCounters.capacity()];
    uint32_t activeCount = 0;
    uint32_t activeSsrcs[_inboundSsrcCounters.capacity()];
    for (auto& it : _inboundSsrcCounters)
    {
        const auto remainingTime = it.second.timeToReceiveReport(timestamp);
        if (remainingTime == 0)
        {
            receiverReportSsrcs[receiverReportCount++] = it.first;
        }
        if (utils::Time::diffLE(it.second.getLastActive(), timestamp, 5 * utils::Time::sec))
        {
            activeSsrcs[activeCount++] = it.first;
        }
    }

    if (senderReportCount == 0 && receiverReportCount <= activeCount / 2 && !rembReady)
    {
        return;
    }

    const auto wallClock = utils::Time::toNtp(std::chrono::system_clock::now());
    const uint64_t ntp32Tick = 0x10000u; // 1/65536 sec

    bool rembAdded = false;
    memory::UniquePacket rtcpPacket;
    uint32_t senderSsrc = 0;
    for (uint32_t i = 0; i < senderReportCount; ++i)
    {
        const uint32_t ssrc = senderReportSsrcs[i];
        auto it = _outboundSsrcCounters.find(ssrc);
        if (it == _outboundSsrcCounters.end())
        {
            assert(false); // we should be alone on this context
            continue;
        }

        if (!rtcpPacket)
        {
            rtcpPacket = memory::makeUniquePacket(_mainAllocator);
            if (!rtcpPacket)
            {
                logger::warn("No space available to send SR", _loggableId.c_str());
                break;
            }
        }

        auto* senderReport = rtp::RtcpSenderReport::create(rtcpPacket->get() + rtcpPacket->getLength());
        senderReport->ssrc = it->first;
        it->second.fillInReport(*senderReport, timestamp, wallClock + i * ntp32Tick);
        for (int k = 0; k < 15 && receiverReportCount > 0; ++k)
        {
            auto receiveIt = _inboundSsrcCounters.find(receiverReportSsrcs[--receiverReportCount]);
            if (receiveIt == _inboundSsrcCounters.end())
            {
                assert(false);
                continue;
            }
            auto& block = senderReport->addReportBlock(receiveIt->first);
            receiveIt->second.fillInReportBlock(timestamp, block, wallClock);
        }
        rtcpPacket->setLength(rtcpPacket->getLength() + senderReport->header.size());
        assert(!memory::PacketPoolAllocator::isCorrupt(rtcpPacket.get()));

        if (!rembAdded)
        {
            appendRemb(*rtcpPacket, timestamp, it->first, activeSsrcs, activeCount);
            rembAdded = true;
        }

        if (rtcpPacket->getLength() + MINIMUM_SR + std::min(receiverReportCount, 4u) * sizeof(rtp::ReportBlock) >
            _config.mtu)
        {
            sendRtcp(std::move(rtcpPacket), timestamp);
        }
    }

    if (_outboundSsrcCounters.size() > 0)
    {
        senderSsrc = _outboundSsrcCounters.begin()->first;
    }

    while (receiverReportCount > 0 || (rembReady && !rembAdded))
    {
        if (rtcpPacket &&
            rtcpPacket->getLength() + MINIMUM_RR + std::min(4u, receiverReportCount) * sizeof(rtp::ReportBlock) >
                _config.mtu)
        {
            sendRtcp(std::move(rtcpPacket), timestamp);
        }

        if (!rtcpPacket)
        {
            rtcpPacket = memory::makeUniquePacket(_mainAllocator);
            if (!rtcpPacket)
            {
                logger::warn("No space available to send RR", _loggableId.c_str());
                break;
            }
        }

        auto* receiverReport = rtp::RtcpReceiverReport::create(rtcpPacket->get() + rtcpPacket->getLength());
        receiverReport->ssrc = senderSsrc;
        for (int k = 0; k < 15 && receiverReportCount > 0; ++k)
        {
            auto receiveIt = _inboundSsrcCounters.find(receiverReportSsrcs[--receiverReportCount]);
            if (receiveIt == _inboundSsrcCounters.end())
            {
                assert(false);
                continue;
            }
            auto& block = receiverReport->addReportBlock(receiveIt->first);
            receiveIt->second.fillInReportBlock(timestamp, block, wallClock);
        }
        rtcpPacket->setLength(receiverReport->header.size());
        assert(!memory::PacketPoolAllocator::isCorrupt(rtcpPacket.get()));

        if (!rembAdded)
        {
            appendRemb(*rtcpPacket, timestamp, senderSsrc, activeSsrcs, activeCount);
            rembAdded = true;
        }

        if (receiverReportCount > 0 &&
            rtcpPacket->getLength() + MINIMUM_RR + std::min(15u, receiverReportCount) * sizeof(rtp::ReportBlock) >
                _config.mtu)
        {
            sendRtcp(std::move(rtcpPacket), timestamp);
        }
    }

    if (rtcpPacket)
    {
        sendRtcp(std::move(rtcpPacket), timestamp);
    }
}

// always add REMB right after SR/RR when there is still space in the packet
void TransportImpl::appendRemb(memory::Packet& rtcpPacket,
    const uint64_t timestamp,
    uint32_t senderSsrc,
    const uint32_t* activeInbound,
    int activeInboundCount)
{
    if (rtcpPacket.getLength() + sizeof(rtp::RtcpRembFeedback) + sizeof(uint32_t) * activeInboundCount >
        _config.rtcp.mtu)
    {
        assert(false);
        return;
    }

    auto& remb = rtp::RtcpRembFeedback::create(rtcpPacket.get() + rtcpPacket.getLength(), senderSsrc);

    if (_config.bwe.logDownlinkEstimates && timestamp - _lastLogTimestamp >= utils::Time::sec * 5)
    {
        const auto oldMin = _inboundMetrics.estimatedKbpsMin.load();
        const auto oldMax = _inboundMetrics.estimatedKbpsMax.load();

        logger::info("Estimates 5s, Downlink %u - %ukbps, rate %.1fkbps, Uplink rctl %.0fkbps, rate %.1fkbps, remb "
                     "%ukbps, rtt %.1fms, pacingQ %zu , rtpProbingEnabled %s",
            _loggableId.c_str(),
            oldMin,
            oldMax,
            _bwe->getReceiveRate(timestamp),
            _rateController.getTargetRate(),
            _sendRateTracker.get(timestamp, utils::Time::ms * 600) * 8 * utils::Time::ms,
            _outboundRembEstimateKbps,
            _rttNtp * 1000.0 / 0x10000,
            _pacingQueue.size() + _rtxPacingQueue.size(),
            _rateController.isRtpProbingEnabled() ? "t" : "f");

        _inboundMetrics.estimatedKbpsMin = 0xFFFFFFFF;
        _inboundMetrics.estimatedKbpsMax = 0;
        _lastLogTimestamp = timestamp;
    }

    const uint64_t mediaBps = _inboundMetrics.estimatedKbps * 1000 * (1.0 - _config.bwe.packetOverhead);
    remb.setBitrate(mediaBps);
    _rtcp.lastReportedEstimateKbps = _inboundMetrics.estimatedKbps;
    for (int i = 0; i < activeInboundCount; ++i)
    {
        remb.addSsrc(activeInbound[i]);
    }
    rtcpPacket.setLength(rtcpPacket.getLength() + remb.header.size());
    assert(!memory::PacketPoolAllocator::isCorrupt(rtcpPacket.get()));
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
            if (senderReport && _config.rctl.enable)
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
    if (_srtpClient && _srtpClient->isInitialized())
    {
        return _srtpClient->unprotect(packet);
    }
    return false;
}

void TransportImpl::removeSrtpLocalSsrc(const uint32_t ssrc)
{
    if (_srtpClient && _srtpClient->isInitialized())
    {
        _srtpClient->removeLocalSsrc(ssrc);
    }
}

bool TransportImpl::setSrtpRemoteRolloverCounter(const uint32_t ssrc, const uint32_t rolloverCounter)
{
    if (_srtpClient && _srtpClient->isInitialized())
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
    const bool rtpComplete = (!_rtpIceSession || _rtpIceSession->getState() == ice::IceSession::State::CONNECTED);
    return rtpComplete && _srtpClient->isDtlsConnected();
}

void TransportImpl::connect()
{
    _jobQueue.addJob<ConnectJob>(*this);
}

void TransportImpl::doConnect()
{
    if (!isInitialized())
    {
        return;
    }

    if (_rtpIceSession)
    {
        _jobQueue.addJob<IceStartJob>(*this, _jobQueue, *_rtpIceSession);
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

std::pair<std::string, std::string> TransportImpl::getLocalCredentials()
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
    memory::PacketPoolAllocator& allocator)
{
    if (_rtpIceSession)
    {
        _jobQueue.addJob<IceSetRemoteJob>(*this, credentials, candidates, allocator);
    }
}

void TransportImpl::doSetRemoteIce(const memory::Packet* credentialPacket,
    const memory::Packet* const* candidatePackets)
{
    if (!_rtpIceSession)
    {
        return;
    }

    auto& credentials = *reinterpret_cast<const IceCredentials*>(credentialPacket->get());
    _rtpIceSession->setRemoteCredentials(std::pair<std::string, std::string>(credentials.ufrag, credentials.password));

    uint32_t candidateCount = 0;
    for (const memory::Packet* const* packetCursor = candidatePackets; *packetCursor; packetCursor++)
    {
        auto& iceCandidates = *reinterpret_cast<const IceCandidates*>((*packetCursor)->get());
        for (size_t i = 0; i < iceCandidates.candidateCount && candidateCount++ < _config.ice.maxCandidateCount; ++i)
        {
            const ice::IceCandidate& candidate = iceCandidates.candidates[i];
            if (candidate.component == ice::IceComponent::RTP && !candidate.empty())
            {
                if (candidate.transportType == ice::TransportType::UDP)
                {
                    _rtpIceSession->addRemoteCandidate(candidate);
                }
                else if (candidate.transportType == ice::TransportType::TCP &&
                    candidate.tcpType == ice::TcpType::PASSIVE)
                {
                    auto endpoints = _tcpEndpointFactory->createTcpEndpoints(candidate.address.getFamily());
                    for (Endpoint* endpoint : endpoints)
                    {
                        endpoint->registerDefaultListener(this);
                        ++_callbackRefCount;
                        _rtpEndpoints.push_back(endpoint);

                        _rtpIceSession->addRemoteCandidate(candidate, endpoint);
                    }
                }
            }
        }
    }
}

void TransportImpl::setRemoteDtlsFingerprint(const std::string& fingerprintType,
    const std::string& fingerprintHash,
    const bool dtlsClientSide)
{
    _dtlsEnabled = true;
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

void TransportImpl::disableDtls()
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

void TransportImpl::onIcePreliminary(ice::IceSession* session,
    ice::IceEndpoint* endpoint,
    const SocketAddress& sourcePort)
{
    if (_selectedRtp == nullptr &&
        (session->getState() == ice::IceSession::State::READY ||
            session->getState() == ice::IceSession::State::CONNECTING))
    {
        logger::debug("switching to %s - %s",
            _loggableId.c_str(),
            endpoint->getLocalPort().toString().c_str(),
            sourcePort.toString().c_str());
        _selectedRtp = static_cast<transport::Endpoint*>(endpoint); // temporary selection
        _peerRtpPort = sourcePort;
        if (_srtpClient->getState() == SrtpClient::State::READY)
        {
            DtlsTimerJob::start(_jobQueue, *this, *_srtpClient, 1);
        }
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
    switch (state)
    {
    case ice::IceSession::State::CONNECTED:
    {
        logger::info("ICE CONNECTED", _loggableId.c_str());

        const auto candidatePair = _rtpIceSession->getSelectedPair();
        const auto rtt = _rtpIceSession->getSelectedPairRtt();
        _rttNtp = utils::Time::absToNtp32(rtt);

        logger::info("Pair selected: %s - %s  rtt:%" PRIu64 "us",
            _loggableId.c_str(),
            candidatePair.first.address.toString().c_str(),
            candidatePair.second.address.toString().c_str(),
            rtt / utils::Time::us);

        for (auto& endpoint : _rtpEndpoints)
        {
            if (endpoint->getState() == Endpoint::State::CONNECTED &&
                endpoint->getLocalPort() == candidatePair.first.baseAddress)
            {
                _selectedRtp = endpoint;
                _selectedRtcp = endpoint;
                _peerRtpPort = candidatePair.second.address;
                _peerRtcpPort = candidatePair.second.address;
            }
            else if (endpoint->getTransportType() == ice::TransportType::TCP)
            {
                endpoint->closePort();
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

const char* dtlsStateToString(const SrtpClient::State state)
{
    switch (state)
    {
    case SrtpClient::State::IDLE:
        return "IDLE";
    case SrtpClient::State::READY:
        return "READY";
    case SrtpClient::State::CONNECTED:
        return "CONNECTED";
    case SrtpClient::State::CONNECTING:
        return "CONNECTING";
    case SrtpClient::State::FAILED:
        return "FAILED";
    default:
        return "unknown";
    }
}

void TransportImpl::onDtlsStateChange(SrtpClient*, const SrtpClient::State state)
{
    logger::info("DTLS %s", getLoggableId().c_str(), dtlsStateToString(state));
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
        logger::debug("sending DTLS protocol message, %d", _loggableId.c_str(), length);
    }

    if (length > memory::Packet::size || length > _config.mtu)
    {
        logger::error("DTLS message %d exceeds MTU %u", _loggableId.c_str(), length, _config.mtu.get());
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

void TransportImpl::setAudioPayloadType(uint8_t payloadType, uint32_t rtpFrequency)
{
    _audio.payloadType = payloadType;
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
    return 0xFFFFu;
}

bool TransportImpl::sendSctp(const uint16_t streamId,
    const uint32_t protocolId,
    const void* data,
    const uint16_t length)
{
    if (!_remoteSctpPort.isSet() || !_sctpAssociation)
    {
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
    _jobQueue.addJob<ConnectSctpJob>(*this);
}

void TransportImpl::doConnectSctp()
{
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

void TransportImpl::setRtxProbeSource(uint32_t ssrc, uint32_t* sequenceCounter)
{
    _rtxProbeSsrc = ssrc;
    _rtxProbeSequenceCounter = sequenceCounter;
    _rateController.setRtpProbingEnabled(!!sequenceCounter);
}

void TransportImpl::runTick(uint64_t timestamp)
{
    if (_pacingInUse.load())
    {
        _jobQueue.addJob<RunTickJob>(*this);
    }
}

void TransportImpl::doRunTick(const uint64_t timestamp)
{
    if (!_config.rctl.enable || !_config.bwe.useUplinkEstimate)
    {
        while (!_rtxPacingQueue.empty())
        {
            protectAndSendRtp(timestamp, _rtxPacingQueue.fetchBack());
        }
        while (!_pacingQueue.empty())
        {
            protectAndSendRtp(timestamp, _pacingQueue.fetchBack());
        }
        _pacingInUse = false;
        return;
    }

    auto budget = _rateController.getPacingBudget(timestamp);
    uint32_t ssrc = 0;
    uint32_t rtpTimestamp = 0;
    while (!_pacingQueue.empty() || !_rtxPacingQueue.empty())
    {
        auto* pacingQueue = &_pacingQueue;
        if (!_rtxPacingQueue.empty())
        {
            pacingQueue = &_rtxPacingQueue;
        }

        if (pacingQueue->back()->getLength() + _config.ipOverhead <= budget)
        {
            memory::UniquePacket packet(pacingQueue->fetchBack());
            budget -= packet->getLength() + _config.ipOverhead;
            auto* rtpHeader = rtp::RtpHeader::fromPacket(*packet);
            ssrc = rtpHeader->ssrc;
            rtpTimestamp = rtpHeader->timestamp;
            protectAndSendRtp(timestamp, std::move(packet));
        }
        else
        {
            break;
        }
    }

    sendPadding(timestamp);

    _pacingInUse = !_pacingQueue.empty() || !_rtxPacingQueue.empty();
}

} // namespace transport
