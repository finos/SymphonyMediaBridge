#pragma once
#include "bwe/BandwidthEstimator.h"
#include "bwe/RateController.h"
#include "config/Config.h"
#include "httpd/HttpDaemon.h"
#include "memory/AudioPacketPoolAllocator.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/ice/IceSession.h"
#include "transport/sctp/SctpConfig.h"

namespace utils
{
class IdGenerator;
class SsrcGenerator;
} // namespace utils

namespace jobmanager
{
class JobManager;
class WorkerThread;
class TimerQueue;
} // namespace jobmanager

namespace transport
{
class RtcePoll;
class SrtpClientFactory;
class SslDtls;
class TransportFactory;
class EndpointFactory;
class ProbeServer;
} // namespace transport

namespace httpd
{
class Httpd;
class HttpDaemonFactory;
} // namespace httpd

namespace bridge
{
class Engine;
class MixerManager;
class ApiRequestHandler;

std::vector<transport::SocketAddress> gatherInterfaces(const config::Config& config);

class Bridge
{
public:
    explicit Bridge(const config::Config& config);
    ~Bridge();

    void initialize();
    void initialize(std::shared_ptr<transport::EndpointFactory> endpointFactory,
        httpd::HttpDaemonFactory& httpdFactory);
    bool isInitialized() const { return _initialized; }

    transport::SslDtls& getSslDtls() { return *_sslDtls; }

private:
    bool _initialized;
    const config::Config _config;
    ice::IceConfig _iceConfig;
    sctp::SctpConfig _sctpConfig;
    bwe::Config _bweConfig;
    bwe::RateControllerConfig _rateControllerConfig;
    const std::unique_ptr<utils::IdGenerator> _idGenerator;
    const std::unique_ptr<utils::SsrcGenerator> _ssrcGenerator;
    std::vector<std::unique_ptr<jobmanager::WorkerThread>> _workerThreads;
    std::unique_ptr<jobmanager::TimerQueue> _timers;
    const std::unique_ptr<jobmanager::JobManager> _jobManager;
    std::vector<transport::SocketAddress> _localInterfaces;
    const std::unique_ptr<transport::SslDtls> _sslDtls;
    std::unique_ptr<transport::SrtpClientFactory> _srtpClientFactory;
    const std::unique_ptr<transport::RtcePoll> _network;
    const std::unique_ptr<memory::PacketPoolAllocator> _mainPacketAllocator;
    const std::unique_ptr<memory::PacketPoolAllocator> _sendPacketAllocator;
    const std::unique_ptr<memory::AudioPacketPoolAllocator> _audioPacketAllocator;
    std::unique_ptr<transport::TransportFactory> _transportFactory;
    std::unique_ptr<transport::ProbeServer> _probeServer;
    const std::unique_ptr<bridge::Engine> _engine;
    std::unique_ptr<bridge::MixerManager> _mixerManager;
    std::unique_ptr<bridge::ApiRequestHandler> _requestHandler;
    std::unique_ptr<httpd::HttpDaemon> _httpd;

    void startWorkerThreads();
};
} // namespace bridge
