#pragma once
#include "bwe/BandwidthEstimator.h"
#include "bwe/RateController.h"
#include "config/Config.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/RtcePoll.h"
#include "transport/dtls/SslDtls.h"
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
} // namespace jobmanager

namespace transport
{
class SrtpClientFactory;
class TransportFactory;
} // namespace transport

namespace httpd
{
class Httpd;
}

namespace bridge
{
class Engine;
class MixerManager;
class LegacyApiRequestHandler;
class ApiRequestHandler;

class Bridge
{
public:
    explicit Bridge(const config::Config& config);
    ~Bridge();

    void initialize();
    bool isInitialized() const { return _initialized; }

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
    const std::unique_ptr<jobmanager::JobManager> _jobManager;
    std::vector<transport::SocketAddress> _localInterfaces;
    const std::unique_ptr<transport::SslDtls> _sslDtls;
    std::unique_ptr<transport::SrtpClientFactory> _srtpClientFactory;
    const std::unique_ptr<transport::RtcePoll> _network;
    const std::unique_ptr<memory::PacketPoolAllocator> _mainPacketAllocator;
    const std::unique_ptr<memory::PacketPoolAllocator> _sendPacketAllocator;
    std::unique_ptr<transport::TransportFactory> _transportFactory;
    const std::unique_ptr<bridge::Engine> _engine;
    std::unique_ptr<bridge::MixerManager> _mixerManager;
    std::unique_ptr<bridge::ApiRequestHandler> _requestHandler;
    std::unique_ptr<httpd::Httpd> _httpd;

    void startWorkerThreads();
};
} // namespace bridge
