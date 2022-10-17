#include "Bridge.h"
#include "aws/AwsHarvester.h"
#include "bridge/ApiRequestHandler.h"
#include "bridge/MixerManager.h"
#include "bridge/engine/Engine.h"
#include "httpd/Httpd.h"
#include "httpd/HttpdFactory.h"
#include "jobmanager/JobManager.h"
#include "jobmanager/WorkerThread.h"
#include "transport/Endpoint.h"
#include "transport/EndpointFactoryImpl.h"
#include "transport/ProbeServer.h"
#include "transport/RtcePoll.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SrtpClientFactory.h"
#include "transport/dtls/SslDtls.h"
#include "utils/IdGenerator.h"
#include "utils/SsrcGenerator.h"

namespace bridge
{

std::vector<transport::SocketAddress> gatherInterfaces(const config::Config& config)
{
    auto defaultIpName = config.ice.preferredIp.get();
    const auto useLocal = defaultIpName == "localhost" || defaultIpName == "127.0.0.1";
    auto interfaces = transport::SocketAddress::activeInterfaces(useLocal, false);
    auto defaultInterface = transport::SocketAddress::parse(defaultIpName);

    std::sort(interfaces.begin(),
        interfaces.end(),
        [defaultInterface, defaultIpName](const transport::SocketAddress& a, const transport::SocketAddress& b) {
            if (a.getFamily() != b.getFamily())
            {
                return a.getFamily() == AF_INET;
            }
            if (a.equalsIp(defaultInterface) || a.getName() == defaultIpName)
            {
                return true;
            }
            return a.toString() < b.toString();
        });

    if (!config.ice.enableIpv6)
    {
        const auto newEnd = std::find_if(interfaces.begin(), interfaces.end(), [](const transport::SocketAddress& a) {
            return a.getFamily() == AF_INET6;
        });
        if (newEnd != interfaces.end())
        {
            interfaces.erase(newEnd, interfaces.end());
        }
    }
    return interfaces;
}

Bridge::Bridge(const config::Config& config)
    : _initialized(false),
      _config(config),
      _idGenerator(std::make_unique<utils::IdGenerator>()),
      _ssrcGenerator(std::make_unique<utils::SsrcGenerator>()),
      _jobManager(std::make_unique<jobmanager::JobManager>()),
      _sslDtls(std::make_unique<transport::SslDtls>()),
      _network(transport::createRtcePoll()),
      _mainPacketAllocator(std::make_unique<memory::PacketPoolAllocator>(32 * 1024, "main")),
      _sendPacketAllocator(std::make_unique<memory::PacketPoolAllocator>(128 * 1024, "send")),
      _audioPacketAllocator(std::make_unique<memory::AudioPacketPoolAllocator>(4 * 1024, "audio")),
      _engine(std::make_unique<bridge::Engine>(config))
{
}

Bridge::~Bridge()
{
    if (_mixerManager)
    {
        _mixerManager->stop();
    }

    _engine->stop();

    _transportFactory.reset(nullptr);

    if (_jobManager)
    {
        _jobManager->stop();
    }
    logger::info("JobManager stopped", "main");

    if (_probeServer)
    {
        _probeServer->stop();
    }

    uint32_t n = 0;
    for (auto& workerThread : _workerThreads)
    {
        workerThread->stop();
        logger::info("stopped workerThread %d", "main", n++);
    }
}

void Bridge::initialize()
{
    httpd::HttpdFactory httpdFactory;
    initialize(std::make_shared<transport::EndpointFactoryImpl>(), httpdFactory);
}

void Bridge::initialize(std::shared_ptr<transport::EndpointFactory> endpointFactory,
    httpd::HttpDaemonFactory& httpdFactory)
{
    _localInterfaces = gatherInterfaces(_config);
    if (_localInterfaces.size() == 0)
    {
        return;
    }
    if (_config.ice.useAwsInfo)
    {
        _iceConfig.publicIpv4 = aws::getPublicIpv4();
        if (!_iceConfig.publicIpv4.empty())
        {
            logger::info("Detected AWS public ip %s", "main", _iceConfig.publicIpv4.ipToString().c_str());
        }
        else
        {
            logger::error("Failed to harvest public ip. Stopping", "main");
            return;
        }
    }
    else if (!_config.ice.publicIpv4.get().empty())
    {
        _iceConfig.publicIpv4 = transport::SocketAddress::parse(_config.ice.publicIpv4);
    }
    if (!_config.ice.publicIpv6.get().empty())
    {
        _iceConfig.publicIpv6 = transport::SocketAddress::parse(_config.ice.publicIpv6);
    }

    startWorkerThreads();

    if (!_sslDtls->isInitialized())
    {
        logger::error("Failed to init SSL. Stopping", "main");
        return;
    }

    _rateControllerConfig.enabled = _config.rctl.enable;
    _rateControllerConfig.ipOverhead = _config.ipOverhead;
    _rateControllerConfig.bandwidthCeilingKbps = _config.rctl.ceiling;
    _rateControllerConfig.bandwidthFloorKbps = _config.rctl.floor;
    _rateControllerConfig.initialEstimateKbps = _config.rctl.initialEstimate;
    _rateControllerConfig.debugLog = _config.rctl.debugLog;

    _srtpClientFactory = std::make_unique<transport::SrtpClientFactory>(*_sslDtls);
    _bweConfig.sanitize();
    _transportFactory = transport::createTransportFactory(*_jobManager,
        *_srtpClientFactory,
        _config,
        _sctpConfig,
        _iceConfig,
        _bweConfig,
        _rateControllerConfig,
        _localInterfaces,
        *_network,
        *_mainPacketAllocator,
        endpointFactory);
    if (!_transportFactory->isGood())
    {
        logger::error("Failed to initialize transport factory", "main");
        return;
    }

    _probeServer = std::make_unique<transport::ProbeServer>(_iceConfig, _config);

    const auto credentials = _probeServer->getCredentials();

    _transportFactory->registerIceListener(*static_cast<transport::Endpoint::IEvents*>(_probeServer.get()),
        credentials.first);

    _transportFactory->registerIceListener(*static_cast<transport::ServerEndpoint::IEvents*>(_probeServer.get()),
        credentials.first);

    _mixerManager = std::make_unique<bridge::MixerManager>(*_idGenerator,
        *_ssrcGenerator,
        *_jobManager,
        *_transportFactory,
        *_engine,
        _config,
        *_mainPacketAllocator,
        *_sendPacketAllocator,
        *_audioPacketAllocator);

    _requestHandler = std::make_unique<bridge::ApiRequestHandler>(*_mixerManager, *_sslDtls, *_probeServer, _config);

    const auto httpAddress = transport::SocketAddress::parse(_config.address, _config.port);
    _httpd = httpdFactory.create(*_requestHandler);
    if (!_httpd->start(httpAddress))
    {
        return;
    }

    _initialized = true;
}

void Bridge::startWorkerThreads()
{
    auto numWorkerThreads = _config.numWorkerTreads.get();
    if (numWorkerThreads == 0)
    {
        const auto hardwareConcurrency = std::thread::hardware_concurrency();
        if (hardwareConcurrency == 0)
        {
            numWorkerThreads = 7;
            logger::warn("Unable to set numWorkerTreads, defaulting to %u", "main", numWorkerThreads);
        }
        else
        {
            numWorkerThreads = std::max(hardwareConcurrency - 1, 1U);
        }
    }
    logger::info("Starting %u worker threads", "main", numWorkerThreads);

    for (int i = 0; i < numWorkerThreads; ++i)
    {
        _workerThreads.push_back(std::make_unique<jobmanager::WorkerThread>(*_jobManager));
    }
}
} // namespace bridge
