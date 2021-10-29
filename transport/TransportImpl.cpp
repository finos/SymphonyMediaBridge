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
#include "sctp/Sctprotocol.h"
#include "transport/DtlsJob.h"
#include "transport/IceJob.h"
#include "transport/SctpJob.h"
#include "transport/TcpEndpoint.h"
#include "utils/ScopedIncrement.h"
#include "utils/SocketAddress.h"
#include "utils/StdExtensions.h"
#include <arpa/inet.h>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

namespace transport
{

// we have to serialize operations on srtp client
// timers, start and receive must be done from same serialized jobmanager.
class PacketReceiveJob : public jobmanager::Job
{
public:
    PacketReceiveJob(TransportImpl& transport,
        Endpoint& endpoint,
        const SocketAddress& source,
        memory::Packet* packet,
        memory::PacketPoolAllocator& allocator,
        void (TransportImpl::*receiveMethod)(Endpoint& endpoint,
            const SocketAddress& source,
            memory::Packet* packet,
            memory::PacketPoolAllocator& allocator,
            uint64_t timestamp))
        : _transport(transport),
          _endpoint(endpoint),
          _packet(packet),
          _allocator(allocator),
          _source(source),
          _timestamp(utils::Time::getAbsoluteTime()),
          _receiveMethod(receiveMethod)
    {
    }

    ~PacketReceiveJob()
    {
        if (_packet)
        {
            _allocator.free(_packet);
        }
    }

    void run() override
    {
        STHREAD_GUARD(_transport._singleThreadMutex);

        (_transport.*_receiveMethod)(_endpoint, _source, _packet, _allocator, _timestamp);
        _packet = nullptr;
    }

private:
    TransportImpl& _transport;
    Endpoint& _endpoint;
    memory::Packet* _packet;
    memory::PacketPoolAllocator& _allocator;
    const SocketAddress _source;
    uint64_t _timestamp;

    void (TransportImpl::*_receiveMethod)(Endpoint&,
        const SocketAddress&,
        memory::Packet*,
        memory::PacketPoolAllocator&,
        uint64_t);
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

struct IceSettings
{
    static const size_t maxCandidateCount = 28;

    // max is actually 256 but usually less
    char ufrag[64];
    char password[128];
    size_t candidateCount = 0;
    ice::IceCandidate candidates[maxCandidateCount];
};
static_assert(sizeof(IceSettings) < sizeof(memory::Packet) - sizeof(size_t), "IceSettings does not fit into a Packet");

class IceSetRemoteJob : public jobmanager::CountedJob
{
public:
    IceSetRemoteJob(TransportImpl& transport,
        const std::pair<std::string, std::string>& credentials,
        const ice::IceCandidates& candidates,
        memory::PacketPoolAllocator& allocator)
        : CountedJob(transport.getJobCounter()),
          _transport(transport),
          _allocator(allocator)
    {
        _iceSettings = memory::makePacket(allocator);
        if (!_iceSettings)
        {
            logger::error("failed to allocate packet", "setRemoteIce");
            return;
        }
        auto& iceSettings = *reinterpret_cast<IceSettings*>(_iceSettings->get());
        utils::strncpy(iceSettings.ufrag, credentials.first.c_str(), sizeof(iceSettings.ufrag));
        utils::strncpy(iceSettings.password, credentials.second.c_str(), sizeof(iceSettings.password));

        for (size_t i = 0; i < IceSettings::maxCandidateCount && i < candidates.size(); ++i)
        {
            iceSettings.candidates[i] = candidates[i];
            iceSettings.candidateCount = i + 1;
        }
    }

    void run() override { _transport.doSetRemoteIce(_iceSettings, _allocator); }

private:
    TransportImpl& _transport;
    memory::PacketPoolAllocator& _allocator;
    memory::Packet* _iceSettings;
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
          _packet(nullptr),
          _allocator(allocator)
    {
        DtlsCredentials credentials;
        utils::strncpy(credentials.fingerprintHash, fingerprintHash, sizeof(credentials.fingerprintHash));
        utils::strncpy(credentials.fingerprintType, fingerprintType, sizeof(credentials.fingerprintType));
        credentials.dtlsClientSide = clientSide;

        _packet = memory::makePacket(allocator, &credentials, sizeof(credentials));
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
        _allocator.free(_packet);
    }

private:
    TransportImpl& _transport;
    SrtpClient& _srtpClient;
    memory::Packet* _packet;
    memory::PacketPoolAllocator& _allocator;
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

        _allocator.free(_packet);
        _packet = nullptr;
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

std::shared_ptr<RtcTransport> createTransport(jobmanager::JobManager& jobmanager,
    SrtpClientFactory& srtpClientFactory,
    const size_t endpointIdHash,
    const config::Config& config,
    const sctp::SctpConfig& sctpConfig,
    const ice::IceConfig& iceConfig,
    ice::IceRole iceRole,
    const bwe::Config& bweConfig,
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
      _outboundSsrcCounters(256),
      _inboundSsrcCounters(16),
      _isRunning(true),
      _sctpConfig(sctpConfig),
      _bwe(std::make_unique<bwe::BandwidthEstimator>(bweConfig)),
      _rtcp(config.rtcp.reportInterval)
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
      _outboundSsrcCounters(256),
      _inboundSsrcCounters(16),
      _isRunning(true),
      _sctpConfig(sctpConfig),
      _bwe(std::make_unique<bwe::BandwidthEstimator>(bweConfig)),
      _rtcp(config.rtcp.reportInterval)
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
    memory::Packet* packet,
    memory::PacketPoolAllocator& allocator)
{
    if (!_jobQueue
             .addJob<PacketReceiveJob>(*this, endpoint, source, packet, allocator, &TransportImpl::internalRtpReceived))
    {
        logger::error("job queue full RTCP", _loggableId.c_str());
        allocator.free(packet);
    }
}

void TransportImpl::internalRtpReceived(Endpoint& endpoint,
    const SocketAddress& source,
    memory::Packet* packet,
    memory::PacketPoolAllocator& allocator,
    uint64_t timestamp)
{
    ++_inboundMetrics.packetCount;
    _inboundMetrics.bytesCount += packet->getLength();

    if (!_srtpClient->isDtlsConnected())
    {
        logger::debug("RTP received, dtls not connected yet", _loggableId.c_str());
        allocator.free(packet);
        return;
    }

    DataReceiver* const dataReceiver = _dataReceiver.load();
    if (!dataReceiver)
    {
        allocator.free(packet);
        return;
    }

    const auto receiveTime = timestamp;
    const auto rtpHeader = rtp::RtpHeader::fromPacket(*packet);
    if (!rtpHeader)
    {
        allocator.free(packet);
        return;
    }

    if (_absSendTimeExtensionId.isSet())
    {
        if (_packetLogger)
        {
            _packetLogger->post(*packet, receiveTime);
        }
        uint32_t absSendTime = 0;
        if (rtp::getTransmissionTimestamp(*packet, _absSendTimeExtensionId.get(), absSendTime))
        {
            doBandwidthEstimation(timestamp, utils::Optional<uint32_t>(absSendTime), packet->getLength());
        }
        else
        {
            doBandwidthEstimation(timestamp, utils::Optional<uint32_t>(), packet->getLength());
        }
    }

    const uint32_t ssrc = rtpHeader->ssrc;
    auto& ssrcState = getInboundSsrc(ssrc); // will do nothing if already exists
    ssrcState.onRtpReceived(*packet, receiveTime);

    const uint32_t extendedSequenceNumber = ssrcState.getExtendedSequenceNumber() -
        static_cast<int16_t>(
            static_cast<uint16_t>(ssrcState.getExtendedSequenceNumber() & 0xFFFFu) - rtpHeader->sequenceNumber.get());

    dataReceiver->onRtpPacketReceived(this, packet, allocator, extendedSequenceNumber, receiveTime);
}

void TransportImpl::onDtlsReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::Packet* packet,
    memory::PacketPoolAllocator& allocator)
{
    if (!_jobQueue.addJob<PacketReceiveJob>(*this,
            endpoint,
            source,
            packet,
            allocator,
            &TransportImpl::internalDtlsReceived))
    {
        logger::error("job queue full DTLS", _loggableId.c_str());
        allocator.free(packet);
    }
}

void TransportImpl::internalDtlsReceived(Endpoint& endpoint,
    const SocketAddress& source,
    memory::Packet* packet,
    memory::PacketPoolAllocator& allocator,
    uint64_t timestamp)
{
    ++_inboundMetrics.packetCount;
    _inboundMetrics.bytesCount += packet->getLength();
    if (packet->get()[0] == DTLSContentType::applicationData)
    {
        DataReceiver* const dataReceiver = _dataReceiver.load();

        if (dataReceiver && _sctpServerPort && _srtpClient->unprotectApplicationData(packet))
        {
            _sctpServerPort->onPacketReceived(packet->get(), packet->getLength(), timestamp);
        }
    }
    else
    {
        logger::debug("received DTLS protocol message, %zu", _loggableId.c_str(), packet->getLength());
        _srtpClient->onMessageReceived(reinterpret_cast<const char*>(packet->get()), packet->getLength());
    }

    allocator.free(packet);
}

void TransportImpl::onRtcpReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::Packet* packet,
    memory::PacketPoolAllocator& allocator)
{
    if (!_jobQueue.addJob<PacketReceiveJob>(*this,
            endpoint,
            source,
            packet,
            allocator,
            &TransportImpl::internalRtcpReceived))
    {
        logger::error("job queue full RTCP", _loggableId.c_str());
        allocator.free(packet);
    }
}

void TransportImpl::internalRtcpReceived(Endpoint& endpoint,
    const SocketAddress& source,
    memory::Packet* packet,
    memory::PacketPoolAllocator& allocator,
    uint64_t timestamp)
{
    ++_inboundMetrics.packetCount;
    _inboundMetrics.bytesCount += packet->getLength();
    if (!_srtpClient->isDtlsConnected())
    {
        logger::debug("RTCP received, dtls not connected yet", _loggableId.c_str());
        allocator.free(packet);
        return;
    }

    auto* dataReceiver = _dataReceiver.load();
    if (!dataReceiver)
    {
        allocator.free(packet);
        return;
    }

    const auto receiveTime = timestamp;
    const auto now = std::chrono::system_clock::now();
    if (unprotect(packet) && rtp::isValidRtcpPacket(*packet))
    {
        rtp::CompoundRtcpPacket compound(packet->get(), packet->getLength());
        for (auto& report : compound)
        {
            processRtcpReport(report, receiveTime, now);
        }

        dataReceiver->onRtcpPacketDecoded(this, packet, allocator, receiveTime);
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
    allocator.free(packet);
}

void TransportImpl::onIceReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::Packet* packet,
    memory::PacketPoolAllocator& allocator)
{
    if (_rtpIceSession)
    {
        if (_rtpIceSession->isRequestAuthentic(packet->get(), packet->getLength()) ||
            _rtpIceSession->isResponseAuthentic(packet->get(), packet->getLength()))
        {
            endpoint.registerListener(source, this);
        }
    }

    if (!_jobQueue
             .addJob<PacketReceiveJob>(*this, endpoint, source, packet, allocator, &TransportImpl::internalIceReceived))
    {
        logger::error("job queue full ICE", _loggableId.c_str());
        allocator.free(packet);
    }
}

void TransportImpl::internalIceReceived(Endpoint& endpoint,
    const SocketAddress& source,
    memory::Packet* packet,
    memory::PacketPoolAllocator& allocator,
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

    allocator.free(packet);
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

void TransportImpl::processRtcpReport(const rtp::RtcpHeader& header,
    const uint64_t timestamp,
    std::__1::chrono::system_clock::time_point wallClock)
{
    const uint32_t RTTMAX = ~0;
    uint32_t rttNtp = RTTMAX;
    uint32_t recvNtp32 = utils::Time::toNtp32(wallClock);

    if (header.packetType == rtp::RtcpPacketType::SENDER_REPORT)
    {
        const auto senderReport = rtp::RtcpSenderReport::fromPtr(&header, header.size());
        for (int i = 0; i < senderReport->header.fmtCount; ++i)
        {
            auto& block = senderReport->reportBlocks[i];
            auto it = _outboundSsrcCounters.find(block.ssrc);
            if (it != _outboundSsrcCounters.cend())
            {
                it->second.onReceiverBlockReceived(timestamp, recvNtp32, block);
            }
        }

        auto& ssrcState = getInboundSsrc(senderReport->ssrc);
        ssrcState.onRtcpReceived(header, timestamp, utils::Time::toNtp(wallClock));
    }
    else if (header.packetType == rtp::RtcpPacketType::RECEIVER_REPORT)
    {
        const auto receiverReport = rtp::RtcpReceiverReport::fromPtr(&header, header.size());
        for (int i = 0; i < receiverReport->header.fmtCount; ++i)
        {
            const auto& block = receiverReport->reportBlocks[i];
            auto it = _outboundSsrcCounters.find(block.ssrc);
            if (it != _outboundSsrcCounters.cend())
            {
                it->second.onReceiverBlockReceived(timestamp, recvNtp32, block);
            }
        }
    }
    else if (rtp::isRemb(&header))
    {
        const auto& remb = reinterpret_cast<const rtp::RtcpRembFeedback&>(header);
        const uint32_t estimatedKbps = remb.getBitRate() / 1000LLU;
        _outboundMetrics.estimatedKbps = estimatedKbps;

        if (_config.bwe.logUplinkEstimates && timestamp - _outboundMetrics.lastLogTimestamp >= utils::Time::sec * 5)
        {
            logger::info("Last uplink estimate, estimate %u kbps", _loggableId.c_str(), estimatedKbps);
            _outboundMetrics.lastLogTimestamp = timestamp;
        }
    }

    if (rttNtp < RTTMAX)
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
        logger::info("unexpected number of inbound streams. Discarding %u", _loggableId.c_str(), nominee->first);
        _inboundSsrcCounters.erase(nominee->first);

        auto pairIt = _inboundSsrcCounters.emplace(ssrc);
        return pairIt.first->second;
    }
    else
    {
        auto pairIt = _inboundSsrcCounters.emplace(ssrc);
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

        auto pairIt = _outboundSsrcCounters.emplace(ssrc, rtpFrequency);
        return pairIt.first->second; // TODO error check
    }
    else
    {
        auto pairIt = _outboundSsrcCounters.emplace(ssrc, rtpFrequency);
        return pairIt.first->second;
    }
}

void TransportImpl::doBandwidthEstimation(const uint64_t receiveTime,
    const utils::Optional<uint32_t>& absSendTime,
    const uint32_t packetSize)
{
    if (!absSendTime.isSet())
    {
        _bwe->onUnmarkedTraffic(packetSize, receiveTime);
        return;
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
        return;
    }

    const uint32_t delta = newEstimate - _rtcp.lastReportedEstimateKbps;
    const uint32_t relativeDelta = delta / std::max(1u, _rtcp.lastReportedEstimateKbps);
    if ((_rtcp.lastSendTime == 0 || utils::Time::diffGT(_rtcp.lastSendTime, receiveTime, _rtcp.reportInterval) ||
            relativeDelta < -0.1 ||
            ((relativeDelta > 0.15 || delta > 500) &&
                utils::Time::diffGT(_rtcp.lastSendTime, receiveTime, _rtcp.reportInterval / 10))))
    {
        appendAndSendRtcp(memory::makePacket(_mainAllocator), _mainAllocator, receiveTime);
    }
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
                ;
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

void TransportImpl::doProtectAndSend(memory::Packet* packet,
    const SocketAddress& target,
    Endpoint* endpoint,
    memory::PacketPoolAllocator& allocator)
{
    assert(packet->getLength() + 24 <= _config.mtu);
    if (endpoint && _srtpClient->protect(packet))
    {
        endpoint->sendTo(target, packet, allocator);
    }
    else
    {
        allocator.free(packet);
    }
}

void TransportImpl::protectAndSend(memory::Packet* packet, memory::PacketPoolAllocator& sendAllocator)
{
    STHREAD_GUARD(_singleThreadMutex);

    assert(_srtpClient);
    const auto timestamp = utils::Time::getAbsoluteTime();

    if (!_srtpClient || !_selectedRtp || !isConnected())
    {
        sendAllocator.free(packet);
        return;
    }

    _outboundMetrics.bytesCount += packet->getLength();
    ++_outboundMetrics.packetCount;

    if (rtp::isRtpPacket(*packet))
    {
        if (_absSendTimeExtensionId.isSet())
        {
            rtp::setTransmissionTimestamp(packet, _absSendTimeExtensionId.get(), timestamp);
        }

        const auto* rtpHeader = rtp::RtpHeader::fromPacket(*packet);
        const auto payloadType = rtpHeader->payloadType;
        const auto isAudio = (payloadType <= 8 || payloadType == _audio.payloadType);
        auto& ssrcState = getOutboundSsrc(rtpHeader->ssrc, isAudio ? _audio.rtpFrequency : 90000);

        ssrcState.onRtpSent(timestamp, *packet);
        doProtectAndSend(packet, _peerRtpPort, _selectedRtp, sendAllocator);

        if (_selectedRtcp &&
            (_rtcp.lastSendTime == 0 || utils::Time::diffGT(_rtcp.lastSendTime, timestamp, _rtcp.reportInterval)))
        {
            appendAndSendRtcp(memory::makePacket(sendAllocator), sendAllocator, timestamp);
        }
    }
    else if (_selectedRtcp)
    {
        if (rtp::isValidRtcpPacket(*packet))
        {
            appendAndSendRtcp(packet, sendAllocator, timestamp);
        }
        else
        {
            logger::error("outbound rtcp report corrupt", _loggableId.c_str());
            sendAllocator.free(packet);
        }
    }
    else
    {
        sendAllocator.free(packet);
    }
}

void TransportImpl::onTriggerRtcp()
{
    const auto timestamp = utils::Time::getAbsoluteTime();
    if (!_selectedRtcp)
    {
        _rtcp.lastSendTime = timestamp;
        return;
    }

    if (utils::Time::diffLT(_rtcp.lastSendTime, timestamp, _rtcp.reportInterval))
    {
        return;
    }

    appendAndSendRtcp(memory::makePacket(_mainAllocator), _mainAllocator, timestamp);
}

memory::Packet* TransportImpl::appendSendReceiveReport(memory::Packet* rtcpPacket,
    memory::PacketPoolAllocator& allocator,
    const uint64_t timestamp,
    const uint32_t* activeInbound,
    int activeInboundCount)
{
    if (!rtcpPacket)
    {
        return nullptr;
    }
    const int MINIMUM_SR = 7 * sizeof(uint32_t);

    const auto wallClock = utils::Time::toNtp(std::chrono::system_clock::now());
    uint32_t senderSsrc = 0;
    if (_rtcp.lastSendTime == 0 || utils::Time::diffGE(_rtcp.lastSendTime, timestamp, _rtcp.reportInterval / 2))
    {
        for (auto& it : _outboundSsrcCounters)
        {
            const size_t remaining = _config.rtcp.mtu - rtcpPacket->getLength();
            if (remaining < MINIMUM_SR + sizeof(rtp::ReportBlock) * activeInboundCount)
            {
                onSendingRtcp(*rtcpPacket, timestamp);
                doProtectAndSend(rtcpPacket, _peerRtcpPort, _selectedRtcp, allocator);
                rtcpPacket = memory::makePacket(allocator);
                if (!rtcpPacket)
                {
                    break;
                }
            }
            if (utils::Time::diffLT(it.second.getLastSendTime(), timestamp, utils::Time::sec * 15))
            {
                senderSsrc = it.first;
                // TODO need reliable mtu source
                auto* senderReport = rtp::RtcpSenderReport::create(rtcpPacket->get() + rtcpPacket->getLength());
                senderReport->ssrc = it.first;
                it.second.fillInReport(*senderReport, timestamp, wallClock);
                while (activeInboundCount > 0)
                {
                    const uint32_t ssrc = activeInbound[activeInboundCount - 1];
                    const auto& counters = _inboundSsrcCounters.find(ssrc)->second;
                    auto& block = senderReport->addReportBlock(ssrc);
                    counters.fillInReportBlock(block, wallClock);
                    --activeInboundCount;
                }
                rtcpPacket->setLength(rtcpPacket->getLength() + senderReport->header.size());
                assert(!memory::PacketPoolAllocator::isCorrupt(rtcpPacket));
            }
        }

        if (rtcpPacket && rtcpPacket->getLength() == 0 && activeInboundCount > 0)
        {
            auto* receiverReport = rtp::RtcpReceiverReport::create(rtcpPacket->get());
            receiverReport->ssrc = senderSsrc;
            while (activeInboundCount > 0)
            {
                const uint32_t ssrc = activeInbound[activeInboundCount - 1];
                const auto& counters = _inboundSsrcCounters.find(ssrc)->second;
                auto& block = receiverReport->addReportBlock(ssrc);
                counters.fillInReportBlock(block, wallClock);
                --activeInboundCount;
            }
            rtcpPacket->setLength(receiverReport->header.size());
            assert(!memory::PacketPoolAllocator::isCorrupt(rtcpPacket));
        }
    }
    return rtcpPacket;
}

memory::Packet* TransportImpl::appendRemb(memory::Packet* rtcpPacket,
    memory::PacketPoolAllocator& allocator,
    const uint64_t timestamp,
    const uint32_t* activeInbound,
    int activeInboundCount)
{
    if (!rtcpPacket)
    {
        return nullptr;
    }
    uint32_t senderSsrc = 0;
    if (rtcpPacket->getLength() > 0)
    {
        auto* header = rtp::RtcpHeader::fromPacket(*rtcpPacket);
        senderSsrc = header->getReporterSsrc();
    }
    if (rtcpPacket->getLength() + sizeof(rtp::RtcpRembFeedback) + sizeof(uint32_t) * activeInboundCount >
        _config.rtcp.mtu)
    {
        onSendingRtcp(*rtcpPacket, timestamp);
        doProtectAndSend(rtcpPacket, _peerRtcpPort, _selectedRtcp, allocator);
        rtcpPacket = memory::makePacket(allocator);
        if (!rtcpPacket)
        {
            return nullptr;
        }
    }
    auto& remb = rtp::RtcpRembFeedback::create(rtcpPacket->get() + rtcpPacket->getLength(), senderSsrc);
    // logger::debug("Sending REMB %.1fkbps %.2f",        _loggableId.c_str(),        _currentEstimateKbps.load(),
    // _bwe.get()->getReceiveRate(timestamp));

    if (_config.bwe.logDownlinkEstimates && timestamp - _inboundMetrics.lastLogTimestamp >= utils::Time::sec * 5)
    {
        const auto oldMin = _inboundMetrics.estimatedKbpsMin.load();
        const auto oldMax = _inboundMetrics.estimatedKbpsMax.load();

        logger::info("Downlink estimates interval 5s, estimate %u - %u kbps, rate %.3f kbps",
            _loggableId.c_str(),
            oldMin,
            oldMax,
            _bwe->getReceiveRate(timestamp));

        _inboundMetrics.estimatedKbpsMin = 0xFFFFFFFF;
        _inboundMetrics.estimatedKbpsMax = 0;
        _inboundMetrics.lastLogTimestamp = timestamp;
    }

    const uint64_t mediaBps = _inboundMetrics.estimatedKbps * 1000 * (1.0 - _config.bwe.packetOverhead);
    remb.setBitRate(mediaBps);
    _rtcp.lastReportedEstimateKbps = _inboundMetrics.estimatedKbps;
    for (int i = 0; i < activeInboundCount; ++i)
    {
        remb.addSsrc(activeInbound[i]);
    }
    rtcpPacket->setLength(rtcpPacket->getLength() + remb.header.size());
    assert(!memory::PacketPoolAllocator::isCorrupt(rtcpPacket));
    return rtcpPacket;
}

void TransportImpl::appendAndSendRtcp(memory::Packet* rtcpPacket,
    memory::PacketPoolAllocator& allocator,
    const uint64_t timestamp)
{
    if (!rtcpPacket || !_selectedRtcp)
    {
        allocator.free(rtcpPacket);
        return;
    }

    uint8_t originalReport[rtcpPacket->getLength()];
    std::memcpy(originalReport, rtcpPacket->get(), rtcpPacket->getLength());
    const int originalReportLength = rtcpPacket->getLength();
    rtcpPacket->setLength(0);

    int activeInboundCount = 0;
    uint32_t activeInbound[_inboundSsrcCounters.capacity()];
    for (auto& it : _inboundSsrcCounters)
    {
        if (utils::Time::diffLT(it.second.getLastActive(), timestamp, utils::Time::sec * 5))
        {
            activeInbound[activeInboundCount++] = it.first;
        }
    }

    rtcpPacket = appendSendReceiveReport(rtcpPacket, allocator, timestamp, activeInbound, activeInboundCount);
    rtcpPacket = appendRemb(rtcpPacket, allocator, timestamp, activeInbound, activeInboundCount);

    if (rtcpPacket && rtcpPacket->getLength() + originalReportLength > _config.rtcp.mtu)
    {
        onSendingRtcp(*rtcpPacket, timestamp);
        doProtectAndSend(rtcpPacket, _peerRtcpPort, _selectedRtcp, allocator);
        rtcpPacket = memory::makePacket(allocator);
    }
    if (rtcpPacket)
    {
        rtcpPacket->append(originalReport, originalReportLength);
        assert(!memory::PacketPoolAllocator::isCorrupt(rtcpPacket));
        onSendingRtcp(*rtcpPacket, timestamp);
        doProtectAndSend(rtcpPacket, _peerRtcpPort, _selectedRtcp, allocator);
        _rtcp.lastSendTime = timestamp;
    }
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
            // rate control will be inserted here
        }

        remainingBytes -= header.size();
    }
}

bool TransportImpl::unprotect(memory::Packet* packet)
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

void TransportImpl::doSetRemoteIce(memory::Packet* packet, memory::PacketPoolAllocator& allocator)
{
    if (_rtpIceSession && packet)
    {
        const auto& iceSettings = *reinterpret_cast<IceSettings*>(packet->get());
        _rtpIceSession->setRemoteCredentials(
            std::pair<std::string, std::string>(iceSettings.ufrag, iceSettings.password));

        for (size_t i = 0; i < iceSettings.candidateCount && i < _config.ice.maxCandidateCount; ++i)
        {
            const ice::IceCandidate& candidate = iceSettings.candidates[i];
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
    allocator.free(packet);
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

int32_t TransportImpl::sendDtls(const char* buffer, const int32_t length)
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

    auto packet = memory::makePacket(_mainAllocator, buffer, length);
    if (packet)
    {
        _selectedRtp->sendTo(_peerRtpPort, packet, _mainAllocator);
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

void TransportImpl::setAbsSendTimeExtensionId(uint8_t extensionId)
{
    _absSendTimeExtensionId.set(extensionId);
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

} // namespace transport
